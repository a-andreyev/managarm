
#include <stdio.h>
#include <assert.h>
#include <functional>
#include <memory>

#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <cofiber.hpp>
#include <boost/intrusive/list.hpp>

#include <bragi/mbus.hpp>
#include <hw.pb.h>

#include "usb.hpp"
#include "uhci.hpp"
#include "hid.hpp"

struct ContiguousPolicy {
public:
	uintptr_t map(size_t length) {
		assert((length % 0x1000) == 0);

		HelHandle memory;
		void *actual_ptr;
		HEL_CHECK(helAllocateMemory(length, kHelAllocContinuous, &memory));
		HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, length,
				kHelMapReadWrite | kHelMapCopyOnWriteAtFork, &actual_ptr));
		HEL_CHECK(helCloseDescriptor(memory));
		return (uintptr_t)actual_ptr;
	}

	void unmap(uintptr_t address, size_t length) {
		HEL_CHECK(helUnmapMemory(kHelNullHandle, (void *)address, length));
	}
};

using ContiguousAllocator = frigg::SlabAllocator<
	ContiguousPolicy,
	frigg::TicketLock
>;

ContiguousPolicy contiguousPolicy;
ContiguousAllocator contiguousAllocator(contiguousPolicy);

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);

enum XferFlags {
	kXferToDevice = 1,
	kXferToHost = 2,
};

struct Endpoint {
	size_t maxPacketSize;	
};

struct Device {
	uint8_t address;
	Endpoint endpoints[32];
};

struct Transaction {
	Transaction(std::shared_ptr<Device> device, int endpoint, XferFlags flags, SetupPacket setup, std::function<void()> callback)
	: _device(device), _endpoint(endpoint), _flags(flags), _completeCounter(0), _setup(setup), _callback(callback) { }

	void buildQueue(void *buffer) {
		assert((_flags & kXferToDevice) || (_flags & kXferToHost));

		size_t max_size = _device->endpoints[_endpoint].maxPacketSize;
		_numTransfers = (_setup.wLength + max_size - 1) / max_size;
		_queue = (QueueHead *)contiguousAllocator.allocate(sizeof(QueueHead));
		_transfers = (TransferDescriptor *)contiguousAllocator.allocate((_numTransfers + 2) * sizeof(TransferDescriptor));
	
		new (_queue) QueueHead;
		_queue->_elementPointer = QueueHead::ElementPointer::from(&_transfers[0]);

		new (&_transfers[0]) TransferDescriptor(TransferStatus(true, false, false),
				TransferToken(TransferToken::kPacketSetup, TransferToken::kData0,
						_device->address, _endpoint, sizeof(SetupPacket)),
				TransferBufferPointer::from(&_setup));
		_transfers[0]._linkPointer = TransferDescriptor::LinkPointer::from(&_transfers[1]);

		size_t progress = 0;
		for(size_t i = 0; i < _numTransfers; i++) {
			size_t chunk = std::min(max_size, _setup.wLength - progress);
			new (&_transfers[i + 1]) TransferDescriptor(TransferStatus(true, false, false),
				TransferToken(_flags & kXferToDevice ? TransferToken::kPacketOut : TransferToken::kPacketIn,
						i % 2 == 0 ? TransferToken::kData0 : TransferToken::kData1,
						_device->address, _endpoint, chunk),
				TransferBufferPointer::from((char *)buffer + progress));
			_transfers[i + 1]._linkPointer = TransferDescriptor::LinkPointer::from(&_transfers[i + 2]);
			progress += chunk;
		}

		new (&_transfers[_numTransfers + 1]) TransferDescriptor(TransferStatus(true, false, false),
				TransferToken(_flags & kXferToDevice ? TransferToken::kPacketIn : TransferToken::kPacketOut,
						TransferToken::kData0, _device->address, _endpoint, 0),
				TransferBufferPointer());
	}

	QueueHead::LinkPointer head() {
		return QueueHead::LinkPointer::from(_queue);
	}

	void linkNext(QueueHead::LinkPointer link) {
		_queue->_linkPointer = link;
	}

	void dumpTransfer() {
		printf("    Setup stage:");
		_transfers[0].dumpStatus();
		printf("\n");
		
		for(size_t i = 0; i < _numTransfers; i++) {
			printf("    Data stage [%lu]:", i);
			_transfers[i + 1].dumpStatus();
			printf("\n");
		}

		printf("    Status stage:");
		_transfers[_numTransfers + 1].dumpStatus();
		printf("\n");
	}

	bool progress() {
//		dumpTransfer();
		
		while(_completeCounter < _numTransfers + 2) {
			TransferDescriptor *transfer = &_transfers[_completeCounter];
			if(transfer->_controlStatus.isActive())
				return false;

			if(transfer->_controlStatus.isAnyError()) {
				printf("Transfer error!\n");
				return true;
			}
			
			_completeCounter++;
		}

		printf("Transfer complete!\n");
		_callback();
		return true;
	}

	boost::intrusive::list_member_hook<> scheduleHook;

private:
	std::shared_ptr<Device> _device;
	int _endpoint;
	XferFlags _flags;
	size_t _completeCounter;
	SetupPacket _setup;
	std::function<void()> _callback;
	size_t _numTransfers;
	QueueHead *_queue;
	TransferDescriptor *_transfers;
};

boost::intrusive::list<
	Transaction, 
	boost::intrusive::member_hook<
		Transaction,
		boost::intrusive::list_member_hook<>,
		&Transaction::scheduleHook
	>
> scheduleList;

// arg0 = wValue in the USB spec
// arg1 = wIndex in the USB spec
struct ControlTransfer {
	ControlTransfer(std::shared_ptr<Device> device, int endpoint, XferFlags flags,
			ControlRecipient recipient, ControlType type, uint8_t request,
			uint16_t arg0, uint16_t arg1, void *buffer, size_t length)
	: device(device), endpoint(endpoint), flags(flags), recipient(recipient), type(type),
			request(request), arg0(arg0), arg1(arg1), buffer(buffer), length(length) { }

	std::shared_ptr<Device> device;
	int endpoint;
	XferFlags flags;
	ControlRecipient recipient;
	ControlType type;
	uint8_t request;
	uint16_t arg0;
	uint16_t arg1;
	void *buffer;
	size_t length;
};

struct Controller {
	Controller(uint16_t base, helx::Irq irq)
	: _base(base), _irq(frigg::move(irq)) { }

	void initialize() {
		auto initial_status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(initial_status & kStatusInterrupt));
		assert(!(initial_status & kStatusError));

		enum {
			kRootConnected = 0x0001,
			kRootConnectChange = 0x0002,
			kRootEnabled = 0x0004,
			kRootEnableChange = 0x0008,
			kRootReset = 0x0200
		};
		
		// global reset, then deassert reset and stop running the frame list
		frigg::writeIo<uint16_t>(_base + kRegCommand, 0x04);
		frigg::writeIo<uint16_t>(_base + kRegCommand, 0);

		// enable interrupts
		frigg::writeIo<uint16_t>(_base + kRegInterruptEnable, 0x0F);

		// disable both ports and clear their connected/enabled changed bits
		frigg::writeIo<uint16_t>(_base + kRegPort1StatusControl,
				kRootConnectChange | kRootEnableChange);
		frigg::writeIo<uint16_t>(_base + kRegPort2StatusControl,
				kRootConnectChange | kRootEnableChange);

		// enable the first port and wait until it is available
		frigg::writeIo<uint16_t>(_base + kRegPort1StatusControl, kRootEnabled);
		while(true) {
			auto port_status = frigg::readIo<uint16_t>(_base + kRegPort1StatusControl);
			if((port_status & kRootEnabled))
				break;
		}

		// reset the first port
		frigg::writeIo<uint16_t>(_base + kRegPort1StatusControl, kRootEnabled | kRootReset);
		frigg::writeIo<uint16_t>(_base + kRegPort1StatusControl, kRootEnabled);
		
		auto postenable_status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(postenable_status & kStatusInterrupt));
		assert(!(postenable_status & kStatusError));

		// setup the frame list
		HelHandle list_handle;
		HEL_CHECK(helAllocateMemory(4096, 0, &list_handle));
		void *list_mapping;
		HEL_CHECK(helMapMemory(list_handle, kHelNullHandle,
				nullptr, 0, 4096, kHelMapReadWrite, &list_mapping));
		
		auto list_pointer = (FrameList *)list_mapping;
		
		for(int i = 0; i < 1024; i++) {
			list_pointer->entries[i] = FrameListPointer::from(&_initialQh);
		}
			
		// pass the frame list to the controller and run it
		uintptr_t list_physical;
		HEL_CHECK(helPointerPhysical(list_pointer, &list_physical));
		assert((list_physical % 0x1000) == 0);
		frigg::writeIo<uint32_t>(_base + kRegFrameListBaseAddr, list_physical);
		
		auto prerun_status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(prerun_status & kStatusInterrupt));
		assert(!(prerun_status & kStatusError));
		
		uint16_t command_bits = 0x1;
		frigg::writeIo<uint16_t>(_base + kRegCommand, command_bits);

		_irq.wait(eventHub, CALLBACK_MEMBER(this, &Controller::onIrq));
	}

	void transfer(ControlTransfer control, std::function<void()> callback) {
		assert((control.flags & kXferToDevice) || (control.flags & kXferToHost));
		
		SetupPacket setup(control.flags & kXferToDevice ? kDirToDevice : kDirToHost,
				control.recipient, control.type, control.request,
				control.arg0, control.arg1, control.length);
		Transaction *transaction = new Transaction(std::move(control.device), control.endpoint,
				control.flags, setup, callback);
		transaction->buildQueue(control.buffer);

		if(scheduleList.empty()) {
			_initialQh._linkPointer = transaction->head();
		}else{
			scheduleList.back().linkNext(transaction->head());
		}
		scheduleList.push_back(*transaction);
	}

	auto erase(Transaction *transaction) {
		auto it = scheduleList.iterator_to(*transaction);

		QueueHead::LinkPointer link;
		if(std::next(it) != scheduleList.end())
			link = std::next(it)->head();

		if(it == scheduleList.begin()) {
			_initialQh._linkPointer = link;
		}else{
			std::prev(it)->linkNext(link);
		}

		return scheduleList.erase(it);
	}

	void onIrq(HelError error) {
		HEL_CHECK(error);

		auto status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(status & 0x10));
		assert(!(status & 0x08));
		if(status & (kStatusInterrupt | kStatusError)) {
			if(status & kStatusError)
				printf("uhci: Error interrupt\n");
			frigg::writeIo<uint16_t>(_base + kRegStatus, kStatusInterrupt | kStatusError);
			
			printf("uhci: Processing transfers.\n");
			auto it = scheduleList.begin();
			while(it != scheduleList.end()) {
				if(it->progress()) {
					it = erase(&(*it));
				}else{
					++it;
				}
			}
		}

		_irq.wait(eventHub, CALLBACK_MEMBER(this, &Controller::onIrq));
	}

private:
	uint16_t _base;
	helx::Irq _irq;

	QueueHead _initialQh;
	alignas(32) uint8_t _buffer[18];
	alignas(32) uint8_t _buffer2[18];
};

struct WaitForXfer {
	friend auto cofiber_awaiter(WaitForXfer action) {
		struct Awaiter {
			Awaiter(std::shared_ptr<Controller> controller, ControlTransfer xfer)
			: _controller(std::move(controller)), _xfer(std::move(xfer)) { }

			bool await_ready() { return false; }
			void await_resume() { }

			void await_suspend(cofiber::coroutine_handle<> handle) {
				_controller->transfer(std::move(_xfer), [handle] () {
					handle.resume();
				});
			}

		private:
			std::shared_ptr<Controller> _controller;
			ControlTransfer _xfer;
		};
	
		return Awaiter(std::move(action._controller), std::move(action._xfer));
	}

	WaitForXfer(std::shared_ptr<Controller> controller, ControlTransfer xfer)
	: _controller(std::move(controller)), _xfer(std::move(xfer)) { }

private:
	std::shared_ptr<Controller> _controller;
	ControlTransfer _xfer;
};

uint32_t fetch(uint8_t *&p, void *limit, int n = 1) {
	uint32_t x = 0;
	for(int i = 0; i < n; i++) {
		x = (x << 8) | *p++;
		assert(p <= limit);
	}
	return x;
}

COFIBER_ROUTINE(cofiber::no_future, parseReportDescriptor(std::shared_ptr<Controller> controller,
		std::shared_ptr<Device> device), [=], {
	size_t length = 52;
	auto buffer = (uint8_t *)contiguousAllocator.allocate(length);
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestInterface, kStandard, SetupPacket::kGetDescriptor, 34 << 8, 0,
			buffer, length));

	uint8_t *p = buffer;
	uint8_t *limit = buffer + length;
	while(p < limit) {
		uint8_t token = fetch(p, limit);
		uint32_t data = fetch(p, limit, token & 0x03);
		switch(token & 0xFC) {
		// Main items
		case 0xC0:
			printf("End Collection: 0x%x\n", data);
			break;

		case 0xA0:
			printf("Collection: 0x%x\n", data);
			break;

		case 0x80:
			printf("Input: 0x%x\n", data);
			break;

		// Global items
		case 0x94:
			printf("Report Count: 0x%x\n", data);
			break;
		
		case 0x74:
			printf("Report Size: 0x%x\n", data);
			break;
		
		case 0x24:
			printf("Logical Maximum: 0x%x\n", data);
			break;
		
		case 0x14:
			printf("Logical Minimum: 0x%x\n", data);
			break;
		
		case 0x04:
			printf("Usage Page: 0x%x\n", data);
			break;

		// Local items
		case 0x28:
			printf("Usage Maximum: 0x%x\n", data);
			break;
		
		case 0x18:
			printf("Usage Minimum: 0x%x\n", data);
			break;
			
		case 0x08:
			printf("Usage: 0x%x\n", data);
			break;

		default:
			printf("Unexpected token: 0x%x\n", token & 0xFC);
			abort();
		}
	}
})

COFIBER_ROUTINE(cofiber::no_future, runHidDevice(std::shared_ptr<Controller> controller), [=], {
	auto device = std::make_shared<Device>();
	device->address = 0;
	device->endpoints[0].maxPacketSize = 8;

	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetAddress, 1, 0,
			nullptr, 0));
	device->address = 1;

	auto descriptor = (DeviceDescriptor *)contiguousAllocator.allocate(sizeof(DeviceDescriptor));
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, 8));
	device->endpoints[0].maxPacketSize = descriptor->maxPacketSize;
	
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, sizeof(DeviceDescriptor)));
	assert(descriptor->length == sizeof(DeviceDescriptor));
	
	auto config = (ConfigDescriptor *)contiguousAllocator.allocate(sizeof(ConfigDescriptor));
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			config, sizeof(ConfigDescriptor)));
	assert(config->length == sizeof(ConfigDescriptor));

	auto buffer = (uint8_t *)contiguousAllocator.allocate(config->totalLength);
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			buffer, config->totalLength));

	auto p = buffer + config->length;
	auto limit = buffer + config->totalLength;
	while(p < limit) {
		auto base = (DescriptorBase *)p;
		p += base->length;

		if(base->descriptorType == kDescriptorInterface) {
			auto desc = (InterfaceDescriptor *)base;
			assert(desc->length == sizeof(InterfaceDescriptor));

			printf("Interface:\n");
			printf("   if num:%d \n", desc->interfaceNumber);	
			printf("   alternate setting:%d \n", desc->alternateSetting);	
			printf("   num endpoints:%d \n", desc->numEndpoints);	
			printf("   if class:%d \n", desc->interfaceClass);	
			printf("   if sub class:%d \n", desc->interfaceSubClass);	
			printf("   if protocoll:%d \n", desc->interfaceProtocoll);	
			printf("   if id:%d \n", desc->iInterface);	
		}else if(base->descriptorType == kDescriptorEndpoint) {
			auto desc = (EndpointDescriptor *)base;
			assert(desc->length == sizeof(EndpointDescriptor));

			printf("Endpoint:\n");
			printf("   endpoint address:%d \n", desc->endpointAddress);	
			printf("   attributes:%d \n", desc->attributes);	
			printf("   max packet size:%d \n", desc->maxPacketSize);	
			printf("   interval:%d \n", desc->interval);
		}else if(base->descriptorType == kDescriptorHid) {
			auto desc = (HidDescriptor *)base;
			assert(desc->length == sizeof(HidDescriptor) + (desc->numDescriptors * sizeof(HidDescriptor::Entry)));
			
			printf("HID:\n");
			printf("   hid class:%d \n", desc->hidClass);
			printf("   country code:%d \n", desc->countryCode);
			printf("   num descriptors:%d \n", desc->numDescriptors);
			printf("   Entries:\n");
			for(size_t entry = 0; entry < desc->numDescriptors; entry++) {
				printf("        Entry %lu:\n", entry);
				printf("        length:%d\n", desc->entries[entry].descriptorLength);
				printf("        type:%d\n", desc->entries[entry].descriptorType);
			}
		}else{
			printf("Unexpected descriptor type: %d!\n", base->descriptorType);
		}
	}

	parseReportDescriptor(controller, device);
})

// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure {
	void operator() ();

private:
	void connected();
	void enumeratedDevice(std::vector<bragi_mbus::ObjectId> objects);
	void queriredDevice(HelHandle handle);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.enumerate({ "pci-vendor:0x8086", "pci-device:0x7020" },
			CALLBACK_MEMBER(this, &InitClosure::enumeratedDevice));
}

void InitClosure::enumeratedDevice(std::vector<bragi_mbus::ObjectId> objects) {
	assert(objects.size() == 1);
	mbusConnection.queryIf(objects[0],
			CALLBACK_MEMBER(this, &InitClosure::queriredDevice));
}

void InitClosure::queriredDevice(HelHandle handle) {
	helx::Pipe device_pipe(handle);

	// acquire the device's resources
	printf("acquire the device's resources\n");
	HelError acquire_error;
	uint8_t acquire_buffer[128];
	size_t acquire_length;
	device_pipe.recvStringRespSync(acquire_buffer, 128, eventHub, 1, 0,
			acquire_error, acquire_length);
	HEL_CHECK(acquire_error);

	managarm::hw::PciDevice acquire_response;
	acquire_response.ParseFromArray(acquire_buffer, acquire_length);

	HelError bar_error;
	HelHandle bar_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 5, bar_error, bar_handle);
	HEL_CHECK(bar_error);

	assert(acquire_response.bars(4).io_type() == managarm::hw::IoType::PORT);
	HEL_CHECK(helEnableIo(bar_handle));

	HelError irq_error;
	HelHandle irq_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 7, irq_error, irq_handle);
	HEL_CHECK(irq_error);
	
	auto controller = std::make_shared<Controller>(acquire_response.bars(4).address(),
			helx::Irq(irq_handle));
	controller->initialize();

	runHidDevice(controller);
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting uhci (usb-)driver\n");

	auto closure = new InitClosure();
	(*closure)();

	while(true)
		eventHub.defaultProcessEvents();
	
	return 0;
}


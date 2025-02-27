#include <iostream>

#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>

#include "controller.hpp"

std::vector<std::unique_ptr<Controller>> globalControllers;

async::detached bindController(mbus::Entity entity) {
	protocols::hw::Device device(co_await entity.bind());	
	auto info = co_await device.getPciInfo();

	auto& ahciBarInfo = info.barInfo[5];
	assert(ahciBarInfo.ioType == protocols::hw::IoType::kIoTypeMemory);
	auto ahciBar = co_await device.accessBar(5);
	auto irq  = co_await device.accessIrq();

	helix::Mapping mapping{ahciBar, ahciBarInfo.offset, ahciBarInfo.length};

	auto controller = std::make_unique<Controller>(std::move(device), std::move(mapping),
			std::move(ahciBar), std::move(irq));
	controller->run();
	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "01"),
		mbus::EqualsFilter("pci-subclass", "06"),
		mbus::EqualsFilter("pci-interface", "01")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		std::cout << "block/ahci: Detected controller\n";
		bindController(std::move(entity));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

int main() {
	std::cout << "block/ahci: Starting driver\n";

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}


#include "kernel.hpp"

namespace thor {

static int64_t nextAsyncId = 1;

int64_t allocAsyncId() {
	int64_t async_id;
	frigg::fetchInc<int64_t>(&nextAsyncId, async_id);
	return async_id;
}

// --------------------------------------------------------
// Debugging and logging
// --------------------------------------------------------

BochsSink infoSink;

// --------------------------------------------------------
// Locking primitives
// --------------------------------------------------------

void IrqSpinlock::lock() {
	irqMutex().lock();
	_spinlock.lock();
}

void IrqSpinlock::unlock() {
	_spinlock.unlock();
	irqMutex().unlock();
}

// --------------------------------------------------------
// Memory management
// --------------------------------------------------------

KernelVirtualMemory::KernelVirtualMemory() {
	// the size is chosen arbitrarily here; 1 GiB of kernel heap is sufficient for now.
	uintptr_t original_base = 0xFFFF'8000'0000'0000;
	size_t original_size = 0x4000'0000;
	
	size_t fine_shift = kPageShift + 4, coarse_shift = kPageShift + 12;
	size_t overhead = frigg::BuddyAllocator::computeOverhead(original_size,
			fine_shift, coarse_shift);
	
	uintptr_t base = original_base + overhead;
	size_t length = original_size - overhead;

	// align the base to the next coarse boundary.
	uintptr_t misalign = base % (uintptr_t(1) << coarse_shift);
	if(misalign) {
		base += (uintptr_t(1) << coarse_shift) - misalign;
		length -= misalign;
	}

	// shrink the length to the next coarse boundary.
	length -= length % (size_t(1) << coarse_shift);

	frigg::infoLogger() << "Kernel virtual memory overhead: 0x"
			<< frigg::logHex(overhead) << frigg::endLog;
	{
		for(size_t offset = 0; offset < overhead; offset += kPageSize) {
			PhysicalAddr physical = physicalAllocator->allocate(0x1000);
			KernelPageSpace::global().mapSingle4k(original_base + offset, physical,
					page_access::write);
		}
	}
	asm("" : : : "memory");
	thorRtInvalidateSpace();

	_buddy.addChunk(base, length, fine_shift, coarse_shift, (void *)original_base);
}

void *KernelVirtualMemory::allocate(size_t length) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	return (void *)_buddy.allocate(length);
}

frigg::LazyInitializer<KernelVirtualMemory> kernelVirtualMemory;

KernelVirtualMemory &KernelVirtualMemory::global() {
	// TODO: This should be initialized at a well-defined stage in the
	// kernel's boot process.
	if(!kernelVirtualMemory)
		kernelVirtualMemory.initialize();
	return *kernelVirtualMemory;
}

KernelVirtualAlloc::KernelVirtualAlloc() { }

uintptr_t KernelVirtualAlloc::map(size_t length) {
	auto p = KernelVirtualMemory::global().allocate(length);

	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = physicalAllocator->allocate(0x1000);
		KernelPageSpace::global().mapSingle4k(VirtualAddr(p) + offset, physical,
				page_access::write);
	}

	asm("" : : : "memory");
	thorRtInvalidateSpace();

	return uintptr_t(p);
}

void KernelVirtualAlloc::unmap(uintptr_t address, size_t length) {
	assert((address % kPageSize) == 0);
	assert((length % kPageSize) == 0);

	asm("" : : : "memory");
	for(size_t offset = 0; offset < length; offset += kPageSize) {
		PhysicalAddr physical = KernelPageSpace::global().unmapSingle4k(address + offset);
		(void)physical;
//	TODO: reeneable this after fixing physical memory allocator
//		physicalAllocator->free(physical);
	}

	thorRtInvalidateSpace();
}

void *KernelAlloc::allocate(size_t size) {
	return _allocator.allocate(size);
}

void KernelAlloc::free(void *pointer) {
	_allocator.free(pointer);
}


frigg::LazyInitializer<PhysicalChunkAllocator> physicalAllocator;
frigg::LazyInitializer<KernelVirtualAlloc> kernelVirtualAlloc;
frigg::LazyInitializer<KernelAlloc> kernelAlloc;

// --------------------------------------------------------
// CpuData
// --------------------------------------------------------

IrqMutex &irqMutex() {
	return getCpuData()->irqMutex;
}

CpuData::CpuData()
: activeFiber{nullptr} { }

// --------------------------------------------------------
// SubmitInfo
// --------------------------------------------------------

SubmitInfo::SubmitInfo()
: asyncId(0), submitFunction(0), submitObject(0) { }

SubmitInfo::SubmitInfo(int64_t async_id,
		uintptr_t submit_function, uintptr_t submit_object)
: asyncId(async_id), submitFunction(submit_function),
		submitObject(submit_object) { }

// --------------------------------------------------------
// Threading related functions
// --------------------------------------------------------

Universe::Universe()
: _descriptorMap(frigg::DefaultHasher<Handle>(), *kernelAlloc), _nextHandle(1) { }

Handle Universe::attachDescriptor(Guard &guard, AnyDescriptor descriptor) {
	assert(guard.protects(&lock));

	Handle handle = _nextHandle++;
	_descriptorMap.insert(handle, frigg::move(descriptor));
	return handle;
}

AnyDescriptor *Universe::getDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));

	return _descriptorMap.get(handle);
}

frigg::Optional<AnyDescriptor> Universe::detachDescriptor(Guard &guard, Handle handle) {
	assert(guard.protects(&lock));
	
	return _descriptorMap.remove(handle);
}

} // namespace thor

// --------------------------------------------------------
// Frigg glue functions
// --------------------------------------------------------

void friggPrintCritical(char c) {
	thor::infoSink.print(c);
}
void friggPrintCritical(char const *str) {
	thor::infoSink.print(str);
}
void friggPanic() {
	thor::disableInts();
	while(true) {
		thor::halt();
	}
}



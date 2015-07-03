
#include "util/hashmap.hpp"

namespace thor {

typedef memory::StupidMemoryAllocator KernelAlloc;

extern LazyInitializer<KernelAlloc> kernelAlloc;

enum Error {
	kErrSuccess = 0
};

typedef uint64_t Handle;

class Universe;
class Channel;

// --------------------------------------------------------
// Memory related classes
// --------------------------------------------------------

class Memory : public SharedObject {
public:
	Memory();

	void resize(size_t length);

	uintptr_t getPage(int index);

private:
	util::Vector<uintptr_t, KernelAlloc> p_physicalPages;
};


// --------------------------------------------------------
// IPC-related classes
// --------------------------------------------------------

// Single producer, single consumer connection
class Channel {
public:
	class Message {
	public:
		Message(char *buffer, size_t length);

		char *getBuffer();
		size_t getLength();

	private:
		char *p_buffer;
		size_t p_length;
	};

	Channel();

	void recvString(char *buffer, size_t length);
	void sendString(const char *buffer, size_t length);

private:
	util::Vector<Message, KernelAlloc> p_messages;
};

class BiDirectionPipe : public SharedObject {
public:
	BiDirectionPipe();

	Channel *getFirstChannel();
	Channel *getSecondChannel();

private:
	Channel p_firstChannel;
	Channel p_secondChannel;
};

// --------------------------------------------------------
// Descriptors
// --------------------------------------------------------

class MemoryAccessDescriptor {
public:
	MemoryAccessDescriptor(SharedPtr<Memory> &&memory);

	UnsafePtr<Memory> getMemory();

private:
	SharedPtr<Memory> p_memory;
};


// Reads from the first channel, writes to the second
class BiDirectionFirstDescriptor {
public:
	BiDirectionFirstDescriptor(SharedPtr<BiDirectionPipe> &&pipe);
	
	void recvString(char *buffer, size_t length);
	void sendString(const char *buffer, size_t length);

private:
	SharedPtr<BiDirectionPipe> p_pipe;
};

// Reads from the second channel, writes to the first
class BiDirectionSecondDescriptor {
public:
	BiDirectionSecondDescriptor(SharedPtr<BiDirectionPipe> &&pipe);
	
	void recvString(char *buffer, size_t length);
	void sendString(const char *buffer, size_t length);

private:
	SharedPtr<BiDirectionPipe> p_pipe;
};

// --------------------------------------------------------
// Process related classes
// --------------------------------------------------------

class AnyDescriptor {
public:
	enum Type {
		kTypeMemoryAccess = 1,
		kTypeBiDirectionFirst = 2,
		kTypeBiDirectionSecond = 3
	};
	
	AnyDescriptor(BiDirectionFirstDescriptor &&descriptor);
	AnyDescriptor(BiDirectionSecondDescriptor &&descriptor);
	AnyDescriptor(MemoryAccessDescriptor &&descriptor);
	
	AnyDescriptor(AnyDescriptor &&other);
	AnyDescriptor &operator= (AnyDescriptor &&other);
	
	Type getType();
	BiDirectionFirstDescriptor &asBiDirectionFirst();
	BiDirectionSecondDescriptor &asBiDirectionSecond();
	MemoryAccessDescriptor &asMemoryAccess();

private:
	Type p_type;
	union {
		BiDirectionFirstDescriptor p_biDirectionFirstDescriptor;
		BiDirectionSecondDescriptor p_biDirectionSecondDescriptor;
		MemoryAccessDescriptor p_memoryAccessDescriptor;
	};
};

class Universe : public SharedObject {
public:
	Universe();

	template<typename... Args>
	Handle attachDescriptor(Args &&... args) {
		Handle handle = p_nextHandle++;
		p_descriptorMap.insert(handle, AnyDescriptor(util::forward<Args>(args)...));
		return handle;
	}

	AnyDescriptor &getDescriptor(Handle handle);

private:
	util::Hashmap<Handle, AnyDescriptor, util::DefaultHasher<Handle>, KernelAlloc> p_descriptorMap;
	Handle p_nextHandle;
};

class AddressSpace : public SharedObject {
public:
	AddressSpace(memory::PageSpace page_space);

	void mapSingle4k(void *address, uintptr_t physical);

private:
	memory::PageSpace p_pageSpace;
};

class Thread : public SharedObject {
public:
	void setup(void *entry, uintptr_t argument);
	
	UnsafePtr<Universe> getNamespace();
	UnsafePtr<AddressSpace> getAddressSpace();
	
	void setUniverse(SharedPtr<Universe> &&universe);
	void setAddressSpace(SharedPtr<AddressSpace> &&address_space);
	
	void switchTo();

private:
	SharedPtr<Universe> p_universe;
	SharedPtr<AddressSpace> p_addressSpace;
	ThorRtThreadState p_state;
};

extern LazyInitializer<SharedPtr<Thread>> currentThread;

} // namespace thor

void *operator new(size_t length, thor::KernelAlloc *);


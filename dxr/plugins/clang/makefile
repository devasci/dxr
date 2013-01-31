LLVM_LIBS := $(shell llvm-config --libdir)
CXXFLAGS := $(shell llvm-config --cflags) -Wall \
	-fno-exceptions -fno-rtti -std=c++11 \
	-I$(shell llvm-config --includedir) $(if $(DEBUG),-O0 -g)
LDFLAGS := -fPIC -g -Wl,-R -Wl,'$$ORIGIN' -L$(LLVM_LIBS) -shared

build: libclang-index-plugin.so

%.o: %.cpp $(wildcard *.h)
	$(CXX) $(CXXFLAGS) -c $< -o $@

libclang-index-plugin.so: dxr-index.o sha1.o dxr-docs.o
	$(CXX) $(LDFLAGS) $^ -o $@ -shared

check: build
	which clang
	which clang++

clean:
	rm -rf *.o libclang-index-plugin.so

.PHONY: build clean

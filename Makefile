.PHONY: all clean

CXX = g++
CXXFLAGS = -std=c++17 -Wall -O0 -g -DDEBUG

all: libhugeargs.so


test: test/build test/build/myecho test/build/myenv libhugeargs.so
	./test/run_test.sh

test/build:
	mkdir -p test/build

test/build/myecho: test/myecho.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

test/build/myenv: test/myenv.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

libhugeargs.so: hugeargs.cpp
	$(CXX) $(CXXFLAGS) -shared -ldl -fPIC -o $@ $<

test/build/test_example: test/example.cpp test/subdir1/foo.h test/subdir2/bar.h
	cd test
	$(CXX) $(CXXFLAGS) -o build/test_example example.cpp -Isubdir1 -Isubdir2
	./build/test_example

clean:
	rm -rf test/build/* libhugeargs.so

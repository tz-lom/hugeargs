.PHONY: all clean

CXX = g++
CXXFLAGS = -std=c++17 -Wall

all: myecho myenv libarghack.so


test: myecho myenv libarghack.so
	./run_large_arg.sh


myecho: myecho.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

myenv: myenv.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

libarghack.so: arghack.cpp
	$(CXX) $(CXXFLAGS) -nostdlib -shared -fPIC -o $@ $<

clean:
	rm -f myecho myenv libarghack.so

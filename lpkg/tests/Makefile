CXX = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -I../src/third_party/include -g
LIBS = -lcurl -larchive -lfmt -lcrypto -lgtest -lgtest_main -lpthread

# Filter out main.o from ../build/ objects
LPKG_OBJS = $(filter-out ../src/build/main.o, $(wildcard ../src/build/*.o))
TEST_SRCS = $(wildcard *.cpp)
TEST_OBJS = $(TEST_SRCS:.cpp=.o)
TARGET = run_tests

all: $(TARGET)

$(TARGET): $(TEST_OBJS) $(LPKG_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(TEST_OBJS) $(TARGET)
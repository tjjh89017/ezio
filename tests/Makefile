GTEST_DIR = $(realpath googletest/googletest/)
CPPFLAGS += -isystem $(GTEST_DIR)/include -I../ -Ilib/
CXXFLAGS += -ltorrent-rasterbar -lboost_system -lstdc++ -lm -lgcc -lssl -lcrypto -lboost_chrono -lboost_random -ldl -lpthread -std=c++11 -g -Wall -Wextra -pthread  

GTEST_HEADERS = $(GTEST_DIR)/include/gtest/*.h \
		$(GTEST_DIR)/include/gtest/internal/*.h
GTEST_SRCS_ = $(GTEST_DIR)/src/*.cc $(GTEST_DIR)/src/*.h $(GTEST_HEADERS)

TEST = all_test
TEST_SRC = $(wildcard *.cpp)
TEST_OBJS = $(patsubst %.cpp,%.o,$(TEST_SRC))
# OBJS = $(patsubst %,../%,$(subst test_,,$(TEST_OBJS)))
OBJS = ../raw_storage.o ../writer.o

.PHONY: all
all: $(TEST)

.PHONY: test
test: $(TEST)
	./$(TEST)

.PHONY: clean
clean:
	rm -f $(TEST) gtest.a gtest_main.a *.o

gtest-all.o: $(GTEST_SRCS_)
	$(CXX) $(CPPFLAGS) -I$(GTEST_DIR) $(CXXFLAGS) -c \
	$(GTEST_DIR)/src/gtest-all.cc

gtest_main.o: $(GTEST_SRCS_)
	$(CXX) $(CPPFLAGS) -I$(GTEST_DIR) $(CXXFLAGS) -c \
	$(GTEST_DIR)/src/gtest_main.cc

gtest.a: gtest-all.o
	$(AR) $(ARFLAGS) $@ $^

gtest_main.a: gtest-all.o gtest_main.o
	$(AR) $(ARFLAGS) $@ $^

$(OBJS):
	make -C ../ $(subst ../,,$@)

%.o: %.cpp $(GTEST_HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST): $(OBJS) $(TEST_OBJS) gtest_main.a
	$(CXX) $^ -o $@ $(CPPFLAGS) $(CXXFLAGS)

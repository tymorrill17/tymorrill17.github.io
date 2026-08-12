CXX      = c++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -I.
LDFLAGS  = -lcmark

ssg: ssg.cpp
	$(CXX) $(CXXFLAGS) -o ssg ssg.cpp $(LDFLAGS)

clean:
	rm -f ssg
	rm -rf _site

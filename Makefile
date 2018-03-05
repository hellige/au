all:
	$(CXX) -std=c++17 -g -o au au.cpp
	./au | od -tcz -tu1

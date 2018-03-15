all:
	mkdir -p out/build
	#cd build && cmake -DCMAKE_CXX_COMPILER=/usr/local/bin/g++-7 -DCMAKE_C_COMPILER=/usr/local/bin/gcc-7 ..
	cd out/build && cmake -DCMAKE_CXX_COMPILER=g++-7 -DCMAKE_C_COMPILER=gcc-7 ../..

.PHONY:all clean reset
compiler=g++
src=ct_test_3.cpp
des=test_out
lib=-lpthread
all:$(des)
$(des):$(src)
	$(compiler) -o $@ $< $(lib) 
clean:
	rm -f $(des) 
reset:
	make clean;make
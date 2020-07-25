kilo: kilo.c
	gcc $^ -o $@

.PHONY:
clean: 
	rm -rf kilo

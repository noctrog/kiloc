kilo: kilo.c
	gcc $^ -o $@

.PHONY clean:
clean: 
	rm -rf kilo

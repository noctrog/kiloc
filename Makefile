kilo: kilo.c
	gcc -g $^ -o $@

.PHONY clean:
clean: 
	rm -rf kilo

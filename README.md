# ConcurrentQueue

ConcurrentQueue is a cyclic and fixed size queue designed mainly for educational purposes, however it can be used in a production as well.

The queue is designed for usage in multithreaded applications, when there are many producers and only a single consumer, as reading from the queue is lock free.

The class is written in C++11 to demonstrate the language's power.
FLAGS  = -Wall -g
LDFLAGS = -pthread -D_REENTRANT
CC     = gcc
PROG   = simplehttpd
OBJS   = simplehttpd.o 

all:	${PROG}

clean:
	rm ${OBJS} ${EXE} *~
  
${PROG}:	${OBJS}
	${CC} ${FLAGS} ${OBJS} -o $@ ${LDFLAGS}

.c.o:
	${CC} ${FLAGS} $< -c

##########################

simplehttpd.o:  simplehttpd.h simplehttpd.c

simplehttpd: simplehttpd.o


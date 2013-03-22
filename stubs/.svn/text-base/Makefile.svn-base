
include ../config.mk

C_COMPILE=$(CC) $(CFLAGS) $(DEFINES)
LINK=

OBJECTS=_stdio.o fmemopen.o fopencookie.o
DEPS=.deps

all:	stubs

clean:
	rm -rf *.o *~ .deps

clobber:
	rm -rf *.o *.P *.d *~ *.a .deps

stubs:	$(OBJECTS)
	$(AR) r fmemopen.a $(OBJECTS) 

%.o : %.c
	@mkdir -p .deps
	$(C_COMPILE) -MD -c $<
	@cp $*.d $*.P; \
	sed -e 's/#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' \
	-e '/^$$/ d' -e 's/$$/ :/' < $*.d >> $*.P; \
	mv $*.P $(DEPS); \
	rm -f $*.d

-include $(OBJECTS:%.o=$(DEPS)/%.P)


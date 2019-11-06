dist_man_MANS = rar2fs.1
if HAVE_GZIP
all : rar2fs.1.gz
rar2fs.1.gz : rar2fs.1
	$(gzip) -9 $< -c > $@

CLEANFILES = rar2fs.1.gz
endif


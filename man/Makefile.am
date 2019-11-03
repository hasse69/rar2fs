if HAVE_GZIP
dist_man_MANS = rar2fs.1.gz
rar2fs.1.gz : rar2fs.1
	$(gzip) -9 $< -c > $@

CLEANFILES = rar2fs.1.gz
else
dist_man_MANS = rar2fs.1
endif


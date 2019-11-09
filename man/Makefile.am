EXTRA_DIST = rar2fs.1
if HAVE_GZIP
all : rar2fs.1.gz
rar2fs.1.gz : rar2fs.1
	$(gzip) -9 $< -c > $@

CLEANFILES = rar2fs.1.gz
endif

install-exec-hook:
	$(MKDIR_P) $(DESTDIR)$(mandir)/man1 && \
	rm -f $(DESTDIR)$(mandir)/man1/rar2fs.*
if HAVE_GZIP
	cp rar2fs.1.gz $(DESTDIR)$(mandir)/man1 && \
	chmod 0644 $(DESTDIR)$(mandir)/man1/rar2fs.1.gz
else
	cp rar2fs.1 $(DESTDIR)$(mandir)/man1 && \
	chmod 0644 $(DESTDIR)$(mandir)/man1/rar2fs.1
endif

uninstall-hook:
	rm -rf $(DESTDIR)$(mandir)/man1/rar2fs.*

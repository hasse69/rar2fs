EXTRA_DIST = rar2fs.1

install-exec-hook:
	$(MKDIR_P) $(DESTDIR)$(mandir)/man1 && \
	rm -f $(DESTDIR)$(mandir)/man1/rar2fs.*
	cp rar2fs.1 $(DESTDIR)$(mandir)/man1 && \
	chmod 0644 $(DESTDIR)$(mandir)/man1/rar2fs.1

uninstall-hook:
	rm -f $(DESTDIR)$(mandir)/man1/rar2fs.*

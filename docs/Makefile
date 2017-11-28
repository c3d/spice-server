TOP=../
MIQ=$(TOP)make-it-quick/

ASCIIDOC=asciidoc
ASCIIDOC_FLAGS=-a icons -a toc

HTML_FILES=spice_style.html spice_threading_model.html
TO_CLEAN+=$(HTML_FILES)

SUBDIRS=manual

include $(MIQ)rules.mk
$(MIQ)rules.mk:
	cd $(TOP) && make $(TARGET)

build: $(HTML_FILES)

%.html: %.txt
	$(PRINT_GENERATE) $(ASCIIDOC) -n $(ASCIIDOC_FLAGS) -o $@ $<

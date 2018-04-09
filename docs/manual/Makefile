TOP=../../

ASCIIDOC=asciidoc
A2X=a2x
ASCIIDOC_FLAGS=-a icons -a toc

DOC_FILES=manual.html manual.chunked
DOC_DIR=$(OBJDIR)manual-chunked/
TO_CLEAN+=$(DOC_FILES)

include make-it-quick/rules.mk

%.html: %.txt
	$(PRINT_GENERATE) $(ASCIIDOC) $(ASCIIDOC_FLAGS) -o $@ $<

manual.chunked: manual.txt $(DOC_DIR).mkdir-only
	$(PRINT_GENERATE) $(A2X) -f chunked -D $(DOC_DIR) $(ASCIIDOC_FLAGS) $<

build: $(DOC_FILES)

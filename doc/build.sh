#!/usr/bin/env bash

rsvg-convert -f pdf -o logo.pdf ../logo.svg

pandoc cover.md UserGuide.md \
	-o UserGuide.pdf \
	--pdf-engine=xelatex \
	--number-sections \
	--highlight-style=espresso \
	-H darkmode.tex \
	-V papersize:letter \
	-V geometry:margin=1in \
	-V fontsize=10pt \
	-V mainfont="DejaVu Serif" \
	-V sansfont="DejaVu Sans" \
	-V monofont="DejaVu Sans Mono"

# Some options for --highlight-style:
#
# pygments
# tango
# espresso
# zenburn
# kate
# monochrome
# breezedark
# haddock

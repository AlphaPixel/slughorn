#!/usr/bin/env bash

rsvg-convert -f pdf -o logo.pdf ../logo.svg

pandoc cover.md UserGuide.md \
	-o UserGuide.pdf \
	--pdf-engine=xelatex \
	--number-sections \
	--highlight-style=breezedark \
	-H darkmode.tex \
	-V papersize:letter \
	-V geometry:margin=1in \
	-V fontsize=11pt \
	-V mainfont="Source Serif 4" \
	-V sansfont="Source Sans 3" \
	-V monofont="Source Code Pro" \
	-V monofontoptions="Scale=MatchLowercase" \
	-V booktabs=true

# Some options for --highlight-style:
#
# espresso
# zenburn
# breezedark

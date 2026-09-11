include($$PWD/branding.env)

# Each shadow build gets its own generated header; no source-tree writes and
# no generator dependency beyond qmake. qmake tracks the inputs for rebuilds.
brand_header.input = $$PWD/src/brand.h.in
brand_header.output = $$OUT_PWD/brand.h
QMAKE_SUBSTITUTES += brand_header
INCLUDEPATH += $$OUT_PWD
HEADERS += $$OUT_PWD/brand.h

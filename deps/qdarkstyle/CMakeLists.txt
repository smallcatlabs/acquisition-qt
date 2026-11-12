add_library(qdarkstyle STATIC
    qdarkstyle/dark/darkstyle.qrc
    qdarkstyle/light/lightstyle.qrc
)

set_target_properties(qdarkstyle PROPERTIES
    AUTORCC ON
)

# Qt6::Core is linked for its usage requirements, not its headers: the
# rcc-generated sources include no Qt header at all, so this target would
# otherwise be compiled with none of Qt's required consumer flags.
#
# Where Qt is built with -DFEATURE_reduce_relocations=ON (Arch and other
# distros), it exports qt_resourceFeatureZstd with protected visibility, and
# the generated resource code references that symbol directly. A direct
# reference forces the linker to emit a copy relocation, which is illegal
# against a protected symbol:
#
#   /usr/bin/ld: libqdarkstyle.a(qrc_lightstyle.cpp.o): copy relocation
#   against non-copyable protected symbol `qt_resourceFeatureZstd@@Qt_6'
#
# Linking Qt6::Core applies the flag that Qt build requires of its consumers
# (-mno-direct-extern-access, or -fPIC on kits without it), routing the
# reference through the GOT so no copy relocation is needed.
target_link_libraries(qdarkstyle PRIVATE Qt6::Core)

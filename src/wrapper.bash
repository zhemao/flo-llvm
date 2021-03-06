mode="release"
if [[ "$1" == "--debug" ]]
then
    mode="debug"
    shift
fi

if [[ "$1" == "--release" ]]
then
    mode="release"
    shift
fi

if [[ "$1" == "--vcdtmp" ]]
then
    mode="vcdtmp"
    shift
fi

if [[ "$1" == "--torture" ]]
then
    mode="torture"
    shift
fi

# If we weren't passed an input then just send the help text.
input="$1"
if [[ "$input" == "" ]]
then
    input="--help"
fi

if [[ "$input" == "--version" ]]
then
    $0-$mode --version
    exit $?
fi

source /etc/lsb-release || true

llvm_link="llvm-link"
opt="opt"
llc="llc"
clang="clang"

# For some crazy reason, Ubuntu has decided to have a clang that's
# actually incompatible with their installed LLVM tools.  I don't know
# why anyone would do such a thing... :(.  To avoid all this I just
# have the Ubuntu package explicitly depend on the 3.3 version of the
# tools and postfix all names.
if [[ "$DISTRIB_DESCRIPTION" == "Ubuntu 12.04.5 LTS" ]]
then
    llvm_link="llvm-link-3.3"
    opt="opt-3.3"
    llc="llc-3.3"
    clang="clang" # That's right, the clang-3.3 package installs clang!
elif test -f /etc/debian_version
then
    # Debian still has old versions of everything...
    llvm_link="llvm-link-3.5"
    opt="opt-3.5"
    llc="llc-3.5"
    clang="clang" # That's right, the clang-3.5 package installs clang!
fi

if [[ "$input" == "--help" ]]
then
    echo "$0 <DESIGN.flo>: Converts Flo files to LLVM IR"
    echo "    The output will be DESIGN.h and DESIGN.o"
    exit 0
fi

tempdir=`mktemp -d -t flo-llvm-wrapper.XXXXXXXXXX`
trap "rm -rf $tempdir" EXIT

$0-$mode "$input" --header > $tempdir/design.h
$0-$mode "$input" --compat > $tempdir/compat.c++
$0-$mode "$input" --ir     > $tempdir/design.llvm

$clang -c -S -emit-llvm \
    -I "$(dirname $input)" \
    -include $tempdir/design.h \
    $tempdir/compat.c++ -o $tempdir/compat.llvm

$llvm_link $tempdir/design.llvm $tempdir/compat.llvm > $tempdir/link.llvm

$opt -O2 $tempdir/link.llvm -o $tempdir/opt.llvm
$llc -O2 $tempdir/opt.llvm -o $tempdir/opt.S

c++ $tempdir/opt.S -c -o $tempdir/opt.o

mv $tempdir/opt.o "$(dirname $input)"/"$(basename $input .flo)".o
mv $tempdir/design.h "$(dirname $input)"/"$(basename $input .flo)".h

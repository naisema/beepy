#!/bin/sh
#
# Rebuild every map pack the navigator uses, from a fresh OpenStreetMap
# extract, and put them on the device.
#
#     tools/mkmaps.sh                 # everything, from download to device
#     tools/mkmaps.sh --no-download   # reuse the .osm.pbf already here
#     tools/mkmaps.sh --no-install    # build only, touch nothing remote
#
# Why a script and not three commands in the README: the three builds are not
# independent. The fine tiles must be cut in the SAME projection frame as the
# coarse ones or tools/mergetiles.py refuses them (see DESIGN.md 6.5), and that
# frame is the centre of the country box -- a number nobody should be
# retyping. Here it is computed from COUNTRY, once, and passed to the build
# that needs it. Get the zoom split wrong instead and there is no error at all,
# just a rung that silently wins over the one you wanted, so that is spelled
# out too.
#
# Roughly 25 minutes, nearly all of it the download. The rendering is seconds.

set -e

# ------------------------------------------------------------ what to build
#
# Where you ride. Full street detail, and the only ground that can be searched
# or routed over -- the road pack is cut from this same extract.
HOME_LL=13.8851,100.3785
HOME_RADIUS=20000        # metres of detail around HOME_LL
HOME_PAD=0.25            # degrees of extract around it; must exceed the radius

# The country, in coarse strokes: major roads only, for when you are 200 km out.
COUNTRY=5.5,97.3,20.6,105.7
PBF_URL=https://download.geofabrik.de/asia/thailand-latest.osm.pbf

# The zoom ladder, split between the two builds. DISJOINT ON PURPOSE: a rung
# in both inputs is taken from the first and the other is discarded, so an
# overlap here would quietly throw away a whole grid.
ZOOMS_FINE=1.5,2.5,4,6,10
ZOOMS_COARSE=15,25,40,60,100,150,250

DEVICE=${DEVICE:-beepy@beepy.local}
SSHKEY=${SSHKEY:-$HOME/.ssh/id_rsa}
OUT=${OUT:-$PWD/maps}
PY=${PY:-python3}

download=1 install=1
for a in "$@"; do
    case $a in
    --no-download) download=0 ;;
    --no-install)  install=0 ;;
    -h|--help)     sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "mkmaps: unknown option $a" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"
cd "$(dirname "$0")/.."

# The frame both tile builds share. It is the centre of COUNTRY because that is
# what mktiles.py picks for a --bbox cut, and 6.1's tangent-plane error grows
# with distance from the reference, so the centre is also simply the right
# place for it.
FRAME=$($PY - "$COUNTRY" <<'EOF'
import sys
la0, lo0, la1, lo1 = (float(v) for v in sys.argv[1].split(","))
print(f"{(la0+la1)/2:g},{(lo0+lo1)/2:g}")
EOF
)

# The extract box around home: the radius plus a margin, because a way is kept
# only if some node of it falls inside, and a road clipped at the box edge ends
# mid-street on the screen.
REGION_BOX=$($PY - "$HOME_LL" "$HOME_PAD" <<'EOF'
import sys
la, lo = (float(v) for v in sys.argv[1].split(","))
p = float(sys.argv[2])
print(f"{la-p:g},{lo-p:g},{la+p:g},{lo+p:g}")
EOF
)

echo "mkmaps: frame $FRAME   region box $REGION_BOX   -> $OUT"

# ------------------------------------------------------------------ extract
#
# A country cannot come from Overpass -- the public API refuses a query that
# size, rightly -- so this starts from Geofabrik, which rebuilds daily.
PBF=$OUT/thailand-latest.osm.pbf
if [ "$download" = 1 ]; then
    echo "mkmaps: downloading $PBF_URL (~310 MB)"
    curl -L --fail --progress-bar -o "$PBF.part" "$PBF_URL"
    mv "$PBF.part" "$PBF"
else
    [ -f "$PBF" ] || { echo "mkmaps: $PBF is not here and --no-download was given" >&2; exit 1; }
fi

# pbf2osm.py is the one step with a dependency outside the standard library
# (pyosmium, which is what reads PBF). It gets its own environment under $OUT
# rather than the ambient python: the alternative is a note in a README saying
# "first pip install osmium", which is exactly the instruction that has rotted
# by the time anyone reads it. Built once; reused every run after.
OSMPY=$PY
if ! $PY -c "import osmium" 2>/dev/null; then
    if [ ! -x "$OUT/.venv/bin/python" ]; then
        echo "mkmaps: pyosmium is not available -- making $OUT/.venv"
        if command -v uv >/dev/null 2>&1; then
            uv venv "$OUT/.venv" >/dev/null
            uv pip install --python "$OUT/.venv/bin/python" osmium >/dev/null
        else
            $PY -m venv "$OUT/.venv"
            "$OUT/.venv/bin/python" -m pip install --quiet --upgrade pip
            "$OUT/.venv/bin/python" -m pip install --quiet osmium
        fi
    fi
    OSMPY=$OUT/.venv/bin/python
fi

# Two conversions, because the two builds want genuinely different data.
# Coarse drops names -- a basemap never draws them and for a country they are
# most of the JSON. The region keeps them: they are what F searches.
echo "mkmaps: country extract, major roads, no names"
$OSMPY tools/pbf2osm.py "$PBF" --bbox "$COUNTRY" --classes coarse --no-names \
    -o "$OUT/country.json"
echo "mkmaps: region extract, every road class, with names and destinations"
$OSMPY tools/pbf2osm.py "$PBF" --bbox "$REGION_BOX" --classes all \
    -o "$OUT/region.json"

# -------------------------------------------------------------------- tiles
echo "mkmaps: coarse tiles"
$PY tools/mktiles.py --osm "$OUT/country.json" --bbox "$COUNTRY" \
    --zooms "$ZOOMS_COARSE" -o "$OUT/coarse.tiles"
echo "mkmaps: fine tiles, in the coarse frame"
$PY tools/mktiles.py --osm "$OUT/region.json" --ref "$HOME_LL" \
    --radius "$HOME_RADIUS" --frame "$FRAME" --zooms "$ZOOMS_FINE" \
    -o "$OUT/fine.tiles"
echo "mkmaps: merging"
$PY tools/mergetiles.py "$OUT/fine.tiles" "$OUT/coarse.tiles" \
    -o "$OUT/thailand-nav.tiles"

# --------------------------------------------------------------- road graph
#
# Separate pack, separate reference, and that is fine: the two are bound to the
# world independently and neither moves the other (asserted by T-MAP-BASEMAP).
# Its extent is the extract's, so search and routing reach exactly as far as
# REGION_BOX -- not as far as the basemap, which is the whole country.
echo "mkmaps: road and name pack"
$PY tools/mkpack.py --osm "$OUT/region.json" --ref "$HOME_LL" \
    -o "$OUT/region.roads"

echo "mkmaps: built"
ls -l "$OUT/thailand-nav.tiles" "$OUT/region.roads"

[ "$install" = 1 ] || exit 0

# ------------------------------------------------------------------- device
#
# Copied to a .new and renamed, so an interrupted transfer never leaves the
# navigator pointed at half a pack. Verified by digest, because a silent
# truncation over wifi looks exactly like a pack with no tiles out there.
echo "mkmaps: installing on $DEVICE"
ssh -i "$SSHKEY" "$DEVICE" 'mkdir -p ~/packs'
for f in thailand-nav.tiles region.roads; do
    scp -i "$SSHKEY" "$OUT/$f" "$DEVICE:packs/$f.new"
    here=$(shasum -a 256 "$OUT/$f" | cut -d' ' -f1)
    there=$(ssh -i "$SSHKEY" "$DEVICE" "sha256sum packs/$f.new" | cut -d' ' -f1)
    [ "$here" = "$there" ] || { echo "mkmaps: $f arrived corrupt" >&2; exit 1; }
    ssh -i "$SSHKEY" "$DEVICE" "mv packs/$f.new packs/$f"
    echo "mkmaps: $f ok ($here)"
done

cat <<EOF

mkmaps: done. ~/.config/beepy-nav.conf should name them:

    basemap    = /home/beepy/packs/thailand-nav.tiles
    roads      = /home/beepy/packs/region.roads

A running navigator holds its packs open -- restart it to see the new data.
EOF

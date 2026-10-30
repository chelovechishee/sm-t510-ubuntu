#!/bin/bash
# Publish the packages of out/debs/ (scripts/make-debs.sh, tested on a
# tablet) in the apt repository: github.com/chelovechishee/sm-t510-ubuntu-apt,
# served by GitHub Pages at https://chelovechishee.github.io/sm-t510-ubuntu-apt
# (docs/maintenance.md).
#
# A version once published is never replaced: the same version with other
# contents is refused (raise the version), the same files are skipped. Each
# package keeps its last few versions, so `apt install t510-kernel=VERSION`
# can go back. The indexes are signed with the key of t510-archive-keyring;
# gpg asks for its passphrase. The repository's branch holds one commit, the
# current state: old packages do not pile up in its history.
#
# Usage: scripts/publish.sh           sign, commit and push (asks first)
#        scripts/publish.sh --local   only make the repository in out/apt
#                                     (T510_APT_DIR=dir for another place)
set -euo pipefail

T=$(cd "$(dirname "$0")/.." && pwd)
D=$T/out/debs
A=${T510_APT_DIR:-$T/out/apt}
REMOTE=git@github.com:chelovechishee/sm-t510-ubuntu-apt.git
URL=https://chelovechishee.github.io/sm-t510-ubuntu-apt
KEY=${T510_KEYRING:-$T/packaging/t510-archive-keyring/t510-archive-keyring.gpg}
SUITE=resolute
KEEP=3
PUSH=1

die() { echo "publish: $*" >&2; exit 1; }
case ${1:-} in
'') ;;
--local) PUSH=0 ;;
*) sed -n '2,/^set /p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 1 ;;
esac

ls "$D"/*.deb >/dev/null 2>&1 || die "no packages in $D: run scripts/make-debs.sh"
[ -f "$KEY" ] || die "no repository key at $KEY"
FPR=$(gpg --show-keys --with-colons "$KEY" | awk -F: '$1 == "fpr" { print $10; exit }')
gpg --list-secret-keys "$FPR" >/dev/null 2>&1 ||
	die "the secret key $FPR of $KEY is not in this gpg"

if [ ! -d "$A" ]; then
	if [ "$PUSH" = 1 ]; then
		git clone -q "$REMOTE" "$A"
	else
		mkdir -p "$A"
		git -C "$A" init -q -b main
	fi
fi
[ "$PUSH" = 0 ] || git -C "$A" remote get-url origin >/dev/null 2>&1 ||
	git -C "$A" remote add origin "$REMOTE"

# what apt reads of a package besides its files: control, scripts, md5sums
meta() { dpkg-deb --ctrl-tarfile "$1" | tar -xO | sha256sum | cut -d' ' -f1; }

added=0
for deb in "$D"/*.deb; do
	name=$(dpkg-deb -f "$deb" Package)
	ver=$(dpkg-deb -f "$deb" Version)
	pool=pool/main/${name:0:1}/$name
	dest=$A/$pool/$(basename "$deb")
	if [ -f "$dest" ]; then
		[ "$(meta "$dest")" = "$(meta "$deb")" ] ||
			die "$name $ver is published with other contents: raise its version"
		echo "  $name $ver: published already"
		continue
	fi
	for old in "$A/$pool"/*.deb; do
		[ -f "$old" ] || continue
		dpkg --compare-versions "$ver" gt "$(dpkg-deb -f "$old" Version)" ||
			die "$name $ver is not newer than the published $(dpkg-deb -f "$old" Version)"
	done
	mkdir -p "$A/$pool"
	cp "$deb" "$dest"
	echo "  $name $ver: new"
	added=$((added + 1))
	# the last $KEEP versions stay
	ls "$A/$pool"/*.deb | while read -r f; do echo "$(dpkg-deb -f "$f" Version) $f"; done |
		sort -V -r | tail -n +$((KEEP + 1)) | while read -r v f; do
			echo "  $name $v: removed (keeping $KEEP versions)"
			rm "$f"
		done
done
[ "$added" -gt 0 ] || [ ! -f "$A/dists/$SUITE/InRelease" ] || { echo "nothing new to publish"; exit 0; }

# indexes
B=dists/$SUITE/main/binary-arm64
rm -rf "$A/dists"
mkdir -p "$A/$B"
(cd "$A" && dpkg-scanpackages --multiversion --arch arm64 pool /dev/null 2>/dev/null) > "$A/$B/Packages"
gzip -9nk "$A/$B/Packages"
{
	echo "Origin: sm-t510-ubuntu"
	echo "Label: sm-t510-ubuntu"
	echo "Suite: $SUITE"
	echo "Codename: $SUITE"
	echo "Date: $(LC_ALL=C date -Ru)"
	echo "Architectures: arm64"
	echo "Components: main"
	echo "Description: Ubuntu on the Samsung Galaxy Tab A 10.1 (2019, SM-T510)"
	echo "SHA256:"
	for f in main/binary-arm64/Packages main/binary-arm64/Packages.gz; do
		printf ' %s %s %s\n' "$(sha256sum < "$A/dists/$SUITE/$f" | cut -d' ' -f1)" \
			"$(stat -c %s "$A/dists/$SUITE/$f")" "$f"
	done
} > "$A/dists/$SUITE/Release"
gpg --batch --yes --local-user "$FPR" --clearsign -o "$A/dists/$SUITE/InRelease" "$A/dists/$SUITE/Release"
gpg --batch --yes --local-user "$FPR" --armor --detach-sign -o "$A/dists/$SUITE/Release.gpg" "$A/dists/$SUITE/Release"
# what apt on a tablet does with it: the key of t510-archive-keyring only
gpgv --keyring "$KEY" "$A/dists/$SUITE/InRelease" 2>/dev/null ||
	die "InRelease does not verify with $KEY"

# for tablets that have no source yet (0.7.0): the key package by a fixed name
cp "$(ls "$A"/pool/main/t/t510-archive-keyring/*.deb | sort -V | tail -n1)" "$A/t510-archive-keyring.deb"
touch "$A/.nojekyll"
cat > "$A/index.html" <<EOF
<!doctype html>
<meta charset="utf-8">
<title>SM-T510 Ubuntu packages</title>
<p>apt repository of <a href="https://github.com/chelovechishee/sm-t510-ubuntu">sm-t510-ubuntu</a>.
Tablets installed from 0.7.1 on have it already. On 0.7.0:</p>
<pre>curl -fLo /tmp/t510-archive-keyring.deb $URL/t510-archive-keyring.deb
sudo apt install /tmp/t510-archive-keyring.deb
sudo apt update
sudo apt install t510-support t510-kernel</pre>
EOF

LIST=$(awk '/^Package:/ { p = $2 } /^Version:/ { print p " " $2 }' "$A/$B/Packages")
echo
echo "$LIST" | sed 's/^/  /'
if [ "$PUSH" = 0 ]; then
	echo "repository made in $A (not pushed)"
	exit 0
fi
read -r -p "Push this to $REMOTE? Type yes: " answer
[ "$answer" = yes ] || die "nothing pushed (the repository is ready in $A)"
git -C "$A" branch -D publish >/dev/null 2>&1 || true
git -C "$A" checkout -q --orphan publish
git -C "$A" add -A
git -C "$A" -c user.name=chelovechishee \
	-c user.email=261262090+chelovechishee@users.noreply.github.com \
	commit -qm "Packages" -m "$LIST"
git -C "$A" branch -M main
git -C "$A" push -q -f origin main
echo "published: $URL (GitHub Pages takes a minute or two)"

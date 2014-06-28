#! /bin/bash

set -x

[ -r "$HOME/.makepkg.conf" ] && . "$HOME/.makepkg.conf"

cd "$(dirname "$0")" &&
. ./PKGBUILD &&
src="src/allegro-$pkgver" &&
rm -rf src pkg &&
mkdir -p "$src" &&
cp -l LICENSE src/ &&
(cd "$OLDPWD" && git ls-files -z | xargs -0 cp -a --parents --target-directory="$OLDPWD/$src") &&
export PACKAGER="`git config user.name` <`git config user.email`>" &&
makepkg --noextract --force &&
rm -rf src pkg &&
sudo pacman -U --noconfirm "$pkgname-$pkgver-$pkgrel-`uname -m`${PKGEXT:-.pkg.tar.xz}"


#!/bin/bash
# asteroids: shrink a mounted iOS 16 root (tools/mkiosroot.sh + sweet-root-tweaks.py)
# so that kernel + root fit fastboot boot's 768 MB on the Nothing Phone (3a).
#
#   sudo bash tools/trim-ios-root.sh /mnt [--dry-run]
#
# Removes what SpringBoard does not need to come up: other languages, voices,
# Siri/dictation assets, wallpapers and most apps. The dyld shared cache stays:
# every process maps it. Run du -sh before and after; keep the untrimmed image.
set -euo pipefail
R=${1:?mounted root}
DRY=${2:-}
[ -d "$R/System/Library/CoreServices" ] || { echo "$R is not an iOS root" >&2; exit 1; }

rmp() {	# remove each existing path, report the size it freed
	for p in "$@"; do
		[ -e "$p" ] || continue
		s=$(du -sm "$p" 2>/dev/null | cut -f1)
		echo "  -${s}M ${p#$R}"
		[ -n "$DRY" ] || rm -rf "$p"
	done
}

echo "before: $(du -sm "$R" | cut -f1) MB"

echo "localizations other than English:"
find "$R/System" "$R/Applications" -type d -name '*.lproj' \
	! -name 'en.lproj' ! -name 'English.lproj' ! -name 'Base.lproj' ! -name 'en_US.lproj' \
	-prune -print0 2>/dev/null | while IFS= read -r -d '' d; do rmp "$d"; done

echo "speech, Siri, dictation, linguistic data:"
rmp "$R/System/Library/TTSResources" "$R/System/Library/PrivateFrameworks/TextToSpeech.framework/Resources" \
	"$R/System/Library/LinguisticData" "$R/System/Library/Assistant" \
	"$R/System/Library/PrivateFrameworks/SiriTTS.framework" "$R/System/Library/AssetsV2"

echo "wallpapers, ringtones, fonts beyond the core set:"
rmp "$R/Library/Wallpaper" "$R/System/Library/Audio/UISounds/New" "$R/Library/Ringtones"
find "$R/System/Library/Fonts" -type f \( -name '*CJK*' -o -name '*Noto*' -o -name 'Apple Color Emoji*' \) \
	-print0 2>/dev/null | while IFS= read -r -d '' f; do rmp "$f"; done

echo "apps (SpringBoard, Settings, Preferences and the system UI stay):"
KEEP='^(Preferences|Setup|SpringBoard|InCallService|SafariViewService|PassbookUIService|Spotlight|FieldTest|Web|MobileSafari)\.app$'
for a in "$R/Applications"/*.app; do
	n=$(basename "$a")
	[[ $n =~ $KEEP ]] || rmp "$a"
done

echo "caches the system rebuilds:"
rmp "$R/System/Library/Caches/com.apple.kernelcaches" "$R/private/var/mobile/Library/Caches"

echo "after: $(du -sm "$R" | cut -f1) MB${DRY:+ (dry run, nothing removed)}"

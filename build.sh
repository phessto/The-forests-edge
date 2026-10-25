#!/usr/bin/env bash
# =============================================================================
# build.sh — build TFE with durable Podman base + build images
# =============================================================================
#
# Usage:
#   ./build.sh                 Use cached images if present; compile + RPM
#   ./build.sh -r | --rebuild  Delete tfe-base (and tfe-build), rebuild both,
#                              prune dangling images, then compile
#   ./build.sh -u | --unit     Run the smoke test inside the build image BEFORE
#                              compile (TFE has no unit tests): build a scratch
#                              copy, boot the world, log in as TempImm, check
#                              areas/rooms loaded, walk, save, shut down from
#                              in game. A failure exits nonzero and skips
#                              compile + RPM (Release is not bumped).
#   ./build.sh -u --unit-only  Smoke test only: no compile, no RPM, no Release
#                              bump, dist/ untouched. Combines with -r / -b.
#   ./build.sh -b | --base B   Builder base: fedora40 (default), ubi8, ubi9
#                              (also BASE=ubi9 ./build.sh). Non-default bases
#                              use images tfe-base-B / tfe-build-B and
#                              output dist/B/ (default stays tfe-base, dist/).
#
# RPM release:
#   Each run increments Release: N%{?dist} in packaging/tfe.spec (Version
#   stays put). The built RPM then sorts newer than the installed one, so
#   rpm -Uvh / dnf install replaces it. Commit the spec when you want that
#   number kept.
#
# Behavior:
#   - cicd/base.oci  → tfe-base   (deps/toolchain only; empty build context)
#   - cicd/build.oci → tfe-build  (FROM base; COPYs cicd/tfe-build;
#                                 context = cicd/)
#   - Compile work lives in the image ENTRYPOINT (/usr/local/bin/tfe-build).
#     This script only: ensure images via podman build, then
#     podman run --rm -v … tfe-build. No podman exec, no inline make.
#   - The entrypoint compiles in a scratch copy under /tmp, so the mounted
#     repo gets no root-owned src/OBJ/ or binaries.
#   - Image builds use --squash-all --rm --force-rm so intermediate layers are
#     not left as separate untagged images. After every successful image build,
#     podman image prune -f removes dangling <none> images.
#   - -r/--rebuild: delete base (+ build), rebuild both (squash + prune), then
#     compile.
#   - -u/--unit: podman run the same image/mounts with TFE_MODE=test.
#   - The compile image is rebuilt automatically when cicd/tfe-build changes
#     (sha256 kept in image label tfe.entrypoint.sha256).
#
# Outputs (./dist/, gitignored):
#   tfe       the MUD server (32-bit, see src/Makefile)
#   daemon    host-name lookup helper that tfe runs as ./daemon
#   rpm/*.rpm binary + source RPM (packaging/tfe.spec; built in-image)
#
# Images: tfe-base / tfe-build  (see cicd/*.oci)
# =============================================================================
set -euo pipefail

# -------------------------------------------------
# Config
# -------------------------------------------------
BASE_IMAGE="tfe-base"
BUILD_IMAGE="tfe-build"
BASE_OCI="cicd/base.oci"
BUILD_OCI="cicd/build.oci"
BUILD_SCRIPT="cicd/tfe-build"
SOURCE_ROOT="$(cd "$(dirname "$0")" && pwd)"
OUTPUT_DIR="${SOURCE_ROOT}/dist"

# -------------------------------------------------
# Helpers
# -------------------------------------------------
log()   { echo -e "\033[32m[+] $1\033[0m"; }
warn()  { echo -e "\033[33m[~] $1\033[0m"; }
die()   { echo -e "\033[31m[!] $1\033[0m" >&2; exit 1; }

need_podman() {
    command -v podman >/dev/null 2>&1 || die "podman not found in PATH"
}

# Bump Release: N%{?dist} so a rebuilt RPM upgrades an already-installed one.
bump_rpm_release() {
    local spec="$SOURCE_ROOT/packaging/tfe.spec"
    local rel n new
    [[ -f "$spec" ]] || die "missing $spec"
    rel="$(awk '/^Release:/{print $2; exit}' "$spec")"
    n="${rel%%%\{?dist\}}"
    [[ "$n" =~ ^[0-9]+$ ]] || die "could not parse Release from $spec (got: ${rel:-empty})"
    new=$((n + 1))
    sed -i -E "s/^(Release:[[:space:]]*)${n}(%\{\?dist\})/\1${new}\2/" "$spec"
    log "RPM release ${n} -> ${new} (Version unchanged; commit packaging/tfe.spec to keep it)"
}

prune_dangling_images() {
    log "Pruning dangling images..."
    podman image prune -f || true
}

# -------------------------------------------------
# Args
# -------------------------------------------------
REBUILD=false
UNIT=false
UNIT_ONLY=false
while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--rebuild) REBUILD=true; shift ;;
        -u|--unit)    UNIT=true; shift ;;
        --unit-only)  UNIT=true; UNIT_ONLY=true; shift ;;
        -b|--base)    [[ $# -ge 2 ]] || die "$1 needs a value"; BASE="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,53p' "$0"
            exit 0
            ;;
        *) die "Unknown: $1 (try --help)" ;;
    esac
done

BASE="${BASE:-fedora40}"
case "$BASE" in
    fedora40) FROM_IMAGE="fedora:40"
              BASE_PKGS="ca-certificates git rpm-build rpmdevtools systemd-rpm-macros" ;;
    ubi8)     FROM_IMAGE="registry.access.redhat.com/ubi8/ubi"
              BASE_PKGS="ca-certificates git rpm-build" ;;
    ubi9)     FROM_IMAGE="registry.access.redhat.com/ubi9/ubi"
              BASE_PKGS="ca-certificates git rpm-build systemd-rpm-macros" ;;
    *) die "Unknown base: $BASE (fedora40, ubi8, ubi9)" ;;
esac
if [[ "$BASE" != fedora40 ]]; then
    BASE_IMAGE="tfe-base-$BASE"
    BUILD_IMAGE="tfe-build-$BASE"
    OUTPUT_DIR="$SOURCE_ROOT/dist/$BASE"
fi

cd "$SOURCE_ROOT"
need_podman

# -------------------------------------------------
# Ensure cicd OCI definitions + in-image build script exist
# -------------------------------------------------
[[ -f "$SOURCE_ROOT/$BASE_OCI" ]]     || die "missing $BASE_OCI (Containerfile for toolchain base)"
[[ -f "$SOURCE_ROOT/$BUILD_OCI" ]]    || die "missing $BUILD_OCI (Containerfile for compile image)"
[[ -f "$SOURCE_ROOT/$BUILD_SCRIPT" ]] || die "missing $BUILD_SCRIPT (compile entrypoint COPYed into tfe-build)"

# -------------------------------------------------
# 0. Fresh output dir
# -------------------------------------------------
if ! $UNIT_ONLY; then
if [[ -d "$OUTPUT_DIR" ]]; then
    log "Removing old output: $OUTPUT_DIR"
    rm -rf "$OUTPUT_DIR"
fi
mkdir -p "$OUTPUT_DIR"
fi

# -------------------------------------------------
# 1. Remove images on full rebuild (-r)
# -------------------------------------------------
if $REBUILD; then
    # Stop/remove any containers still using the old images before rmi.
    for img in "$BUILD_IMAGE" "$BASE_IMAGE"; do
        podman ps -q --filter "ancestor=$img" --filter "status=running" \
            | xargs -r podman kill || true
        podman ps -a -q --filter "ancestor=$img" \
            | xargs -r podman rm -f || true
    done
    # Also catch ephemeral compile containers named tfe-build-*
    podman ps -a -q --filter "name=^tfe-build-" \
        | xargs -r podman rm -f || true

    for img in "$BUILD_IMAGE" "$BASE_IMAGE"; do
        if podman image exists "$img"; then
            log "Removing image for full rebuild: $img"
            podman rmi -f "$img" >/dev/null || true
        fi
    done
fi

# -------------------------------------------------
# 2. Create base + build images if missing
# -------------------------------------------------
# Base: empty context so nothing from the repo can land in the image layers.
# Build: context = cicd/ so COPY tfe-build works (still no project tree).
# --squash-all collapses layers so intermediates are not left as <none> images.
# Prune after every successful build so retags do not orphan old IDs.
build_base_image() {
    local empty_ctx
    empty_ctx="$(mktemp -d "${TMPDIR:-/tmp}/tfe-buildctx.XXXXXX")"
    if ! podman build \
        --squash-all \
        --rm \
        --force-rm \
        --build-arg "BASE_IMAGE=$FROM_IMAGE" \
        --build-arg "BASE_PKGS=$BASE_PKGS" \
        -t "$BASE_IMAGE" \
        -f "$SOURCE_ROOT/$BASE_OCI" \
        "$empty_ctx"
    then
        rm -rf "$empty_ctx"
        die "podman build failed for $BASE_IMAGE (-f $BASE_OCI)"
    fi
    rm -rf "$empty_ctx"
    prune_dangling_images
}

build_compile_image() {
    if ! podman build \
        --squash-all \
        --rm \
        --force-rm \
        --build-arg "BASE_TAG=$BASE_IMAGE" \
        --label "tfe.entrypoint.sha256=$ENTRY_SHA" \
        -t "$BUILD_IMAGE" \
        -f "$SOURCE_ROOT/$BUILD_OCI" \
        "$SOURCE_ROOT/cicd"
    then
        die "podman build failed for $BUILD_IMAGE (-f $BUILD_OCI)"
    fi
    prune_dangling_images
}

if ! podman image exists "$BASE_IMAGE"; then
    log "Creating base image $BASE_IMAGE from $BASE_OCI ($FROM_IMAGE + g++ -m32 toolchain + rpmbuild)"
    log "  (no project COPY — empty build context; source is mounted at compile time)"
    build_base_image
else
    log "Using cached base image: $BASE_IMAGE"
fi

ENTRY_SHA="$(sha256sum "$SOURCE_ROOT/$BUILD_SCRIPT" | awk '{print $1}')"
if podman image exists "$BUILD_IMAGE"; then
    have_sha="$(podman image inspect --format '{{ index .Labels "tfe.entrypoint.sha256" }}' "$BUILD_IMAGE" 2>/dev/null || true)"
    if [[ "$have_sha" != "$ENTRY_SHA" ]]; then
        log "$BUILD_SCRIPT changed since $BUILD_IMAGE was built; rebuilding compile image"
        podman rmi -f "$BUILD_IMAGE" >/dev/null || true
    fi
fi
if ! podman image exists "$BUILD_IMAGE"; then
    log "Creating compile image $BUILD_IMAGE from $BUILD_OCI (FROM $BASE_IMAGE; COPY $BUILD_SCRIPT)"
    build_compile_image
else
    log "Using cached compile image: $BUILD_IMAGE"
fi

# -------------------------------------------------
# 3. Run build image: mounts only; compile is ENTRYPOINT inside the image
# -------------------------------------------------
# A squashed image can lack /tmp; the scratch compile, smoke test and
# rpmbuild need it (and the smoke test runs tfe from it, so not noexec).
TMP_MOUNT=(--tmpfs /tmp:rw,exec,nosuid,nodev,mode=1777)
if $UNIT; then
    log "Running smoke test in $BUILD_IMAGE (TFE_MODE=test)..."
    if ! podman run --rm \
        --name "tfe-build-test-$(date +%s)-$$" \
        -e TFE_MODE=test \
        "${TMP_MOUNT[@]}" \
        -v "$SOURCE_ROOT":/src:Z \
        "$BUILD_IMAGE"
    then
        die "Smoke test failed in $BUILD_IMAGE ($BASE); compile and RPM skipped"
    fi
    log "Smoke test passed ($BASE)"
    if $UNIT_ONLY; then
        log "--unit-only: skipping compile and RPM"
        exit 0
    fi
fi

bump_rpm_release

log "Compiling binaries + RPM into $OUTPUT_DIR via $BUILD_IMAGE ENTRYPOINT..."
podman run --rm \
    --name "tfe-build-$(date +%s)-$$" \
    "${TMP_MOUNT[@]}" \
    -v "$SOURCE_ROOT":/src:Z \
    -v "$OUTPUT_DIR":/out:Z \
    "$BUILD_IMAGE"

# -------------------------------------------------
# Done
# -------------------------------------------------
log "Build complete."
echo "  $OUTPUT_DIR/tfe"
echo "  $OUTPUT_DIR/daemon"
if [[ -d "$OUTPUT_DIR/rpm" ]]; then
    # shellcheck disable=SC2012
    ls -1 "$OUTPUT_DIR"/rpm/*.rpm 2>/dev/null | sed 's/^/  /' || true
fi
echo ""
echo "Install: dnf install $OUTPUT_DIR/rpm/tfe-*.x86_64.rpm   (pulls 32-bit glibc/libstdc++/libssh)"
echo "Then:    edit /etc/tfe/tfe.env (TFE_SEED_TEST_LOGIN=yes for the TempImm/llama test login),"
echo "         systemctl enable --now tfe.service   (telnet <host> 4000)"
echo "SSH:     off until TFE_SSH_PORT is set in /etc/tfe/tfe.env (e.g. 4022; then ssh -p 4022 play@<host>)"
echo ""
echo "If you still see dangling <none> images after an older build:"
echo "  podman image prune -f && ./build.sh -r"

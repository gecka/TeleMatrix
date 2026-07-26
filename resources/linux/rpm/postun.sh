# RPM %postun scriptlet body. $1 is the count of remaining versions:
# 0 = final erase (clean up), >0 = upgrade (leave links in place).
if [ "$1" = "0" ]; then
    rm -f /usr/bin/telematrix
    rm -f /usr/share/applications/dev.telematrix.TeleMatrix.desktop
    rm -f /usr/share/metainfo/dev.telematrix.TeleMatrix.metainfo.xml
    for sz in 16 32 48 64 128 256; do
        rm -f "/usr/share/icons/hicolor/${sz}x${sz}/apps/dev.telematrix.TeleMatrix.png"
    done
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
    gtk-update-icon-cache -q -f /usr/share/icons/hicolor 2>/dev/null || true
fi
exit 0

# macOS packaging: turns the supersaga executable into SuperSaga.app (ad-hoc signed) and adds a `dmg`
# target. Never put the ROM, a BIOS or anything derived from them into the bundle.
function(supersaga_configure_bundle target)
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "SuperSaga"
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "tech.mcclain.supersaga"
        MACOSX_BUNDLE_BUNDLE_NAME "SuperSaga"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")

    # App icon, drawn procedurally by supersaga_icon (original artwork).
    set(icon "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns")
    add_custom_command(OUTPUT "${icon}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset"
        COMMAND supersaga_icon "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset"
        COMMAND iconutil -c icns -o "${icon}" "${CMAKE_CURRENT_BINARY_DIR}/AppIcon.iconset"
        DEPENDS supersaga_icon
        COMMENT "Rendering AppIcon.icns")
    target_sources(${target} PRIVATE "${icon}")
    set_source_files_properties("${icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

    # License and third-party notices.
    set(lic "${PROJECT_SOURCE_DIR}/assets/fonts/DMSans-OFL.txt" "${PROJECT_SOURCE_DIR}/assets/fonts/Lucide-LICENSE.txt")
    target_sources(${target} PRIVATE ${lic})
    set_source_files_properties(${lic} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses/fonts)
    set(notices "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
    target_sources(${target} PRIVATE ${notices})
    set_source_files_properties(${notices} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses)

    # RecompLauncher metadata, copied unchanged when the repository has it.
    if(EXISTS "${PROJECT_SOURCE_DIR}/recomplauncher.json")
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/recomplauncher.json")
        set_source_files_properties("${PROJECT_SOURCE_DIR}/recomplauncher.json" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND xattr -cr "$<TARGET_BUNDLE_DIR:${target}>"
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:${target}>"
        COMMENT "Ad-hoc signing SuperSaga.app")

    set(dmg "${CMAKE_CURRENT_BINARY_DIR}/SuperSaga-${PROJECT_VERSION}-macOS.dmg")
    set(stage "${CMAKE_CURRENT_BINARY_DIR}/dmg-stage")
    add_custom_target(dmg
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${stage}" "${dmg}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage}"
        COMMAND cp -R "$<TARGET_BUNDLE_DIR:${target}>" "${stage}/"
        COMMAND ln -s /Applications "${stage}/Applications"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PROJECT_SOURCE_DIR}/packaging/README-macOS.txt" "${stage}/Read Me.txt"
        COMMAND "${CMAKE_COMMAND}" -E copy "${PROJECT_SOURCE_DIR}/LICENSE" "${stage}/LICENSE.txt"
        COMMAND hdiutil create -volname "SuperSaga" -srcfolder "${stage}" -ov -format UDZO "${dmg}"
        DEPENDS ${target}
        COMMENT "Building ${dmg}"
        VERBATIM)
endfunction()

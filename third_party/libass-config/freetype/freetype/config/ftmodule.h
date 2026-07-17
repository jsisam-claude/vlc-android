/* Hand-authored module list for the vendored FreeType build (VER-2-14-3).
 * Replaces include/freetype/config/ftmodule.h. Enabled modules match the
 * vendored src/ subset exactly: TrueType, CFF and Type 1 font drivers, the
 * SFNT container, PostScript aux/names/hinter, the autofitter, and the
 * smooth (anti-aliased) and raster (monochrome) rasterizers.
 * Dropped vs upstream default: t1cid, pfr, type42, winfonts, pcf, bdf. */
FT_USE_MODULE( FT_Module_Class, autofit_module_class )
FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, t1_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_raster1_renderer_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )
/* EOF */

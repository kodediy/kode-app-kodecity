# The game's engine is portable C in engine/; the board side is the renderer
# (an Active Object with kscreen) and the tasks in kodecity_main.c.
idf_component_register(
    SRCS "kodecity_main.c" "kodecity_ao.c" "kodecity_frame.c"
         "engine/game.c" "engine/world.c" "engine/r3d.c" "engine/sound.c" "engine/font5x7.c"
    INCLUDE_DIRS "." "engine"
    REQUIRES kode kscreen
    PRIV_REQUIRES esp_timer esp_mm
)
# The board builds for debugging (-Og); the game's software renderer needs
# the optimiser, so this component alone is built with -O2.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffast-math)

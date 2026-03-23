if(CONFIG_MW_AUDIO_ENABLE)
    set(AUDIO_SRCS
        "pwm_audio.c"
        "mixer.c"
        "audio_player.c"
        "audio_src_flash.c"
        "micromod.c"
        "mod_player.c"
        "helix/mp3dec.c"
        "helix/mp3tabs.c"
        "helix/bitstream.c"
        "helix/buffers.c"
        "helix/dct32.c"
        "helix/dequant.c"
        "helix/dqchan.c"
        "helix/huffman.c"
        "helix/hufftabs.c"
        "helix/imdct.c"
        "helix/polyphase.c"
        "helix/scalfact.c"
        "helix/stproc.c"
        "helix/subband.c"
        "helix/trigtabs.c"
    )
else()
    set(AUDIO_SRCS)
endif()

idf_component_register(
    SRCS ${AUDIO_SRCS}
    INCLUDE_DIRS
        "include"
        "."
        "helix"
    REQUIRES
        driver
        esp_driver_ledc
        esp_driver_gptimer
        freertos
        log
)

if(CONFIG_MW_AUDIO_ENABLE)
    target_add_binary_data(${COMPONENT_LIB} "test.mp3" BINARY)
    target_add_binary_data(${COMPONENT_LIB} "test.mod" BINARY)
endif()

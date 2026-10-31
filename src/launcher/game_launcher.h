#pragma once

// The game lives in its own firmware (env waveshare-knob-1_8-doom), installed
// in the flash's factory slot beside the dial's two update slots -- see
// partitions_dial_game.csv. Updates never touch the factory slot, so a dial
// update can't overwrite the game.
//
// Launching points the chip at the game and restarts. The game points it
// straight back at the dial as it starts, so any restart -- leaving the game,
// a crash, the power going -- lands back here.
namespace GameLauncher
{
    // The game's firmware is present in its slot.
    bool installed();

    // Why the game can't be started right now, or nullptr if it can. While
    // the game runs the dial is off the network, so nothing the dial is
    // driving may be under way -- and whether FluidNC minds the websocket
    // dropping mid-job is untested, so a job blocks it too.
    const char *blockedReason();

    // Restart into the game. Doesn't return if it works; returns false if
    // the game isn't installed or the slot couldn't be selected.
    bool launch();
}

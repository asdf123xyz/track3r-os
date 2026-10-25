# OS-QANET E1: what an owner of the official firmware expects

This file was written by an AI agent asked to take the view of a long-time owner of the Polyend Tracker who has used the official firmware for years. It comes only from the agent's general user knowledge of that device and of trackers and samplers in general. No project document, specification, UI map, QA catalogue, user manual or source code of this repository was read, and neither the official manual nor any web page was looked up. One item (E-LOAD-01) was given by the maintainer. **Every item is an `open research question` until the maintainer confirms it**; nothing here is `verified parity`. The items are meant to be compared, one by one, with what the Community OS does.

How to read an item:

- **Confidence**: `sure` (an owner would be very surprised to be wrong), `likely` (remembered, details may differ), `unsure` (a vague memory or a guess).
- **Source**: `OG` means the agent recalls it as behaviour of the official firmware; `conv` means a general tracker or sampler convention that an owner would carry over, not specific knowledge of the official firmware; `maintainer` means the maintainer stated it.
- Keys: `F1`..`F8` (the eight keys under the screen), `Shift`, the arrows, the encoder, the 48 pads (4 rows of 12), `Play`, `Rec`, `Copy`, `Insert`, `Delete`, and the screen buttons. "Shift + X" means holding Shift and pressing X.
- When an item says "warns" it means a confirmation popup that must be answered before anything is lost.

## 1. Sample Load (the sample browser)

- **E-LOAD-01** Holding `Play` auditions the highlighted file; while `Play` is held, moving to another file plays the new file at once, and the audition starts with no noticeable wait. *(sure · maintainer)*
- **E-LOAD-02** Releasing `Play` stops the audition; the audition never loads anything into an instrument and never changes the project. *(likely · OG)*
- **E-LOAD-03** The screen shows two lists side by side: the SD card browser (folders and WAV files) and the instrument slots 1..48 with the name of what each slot holds. *(likely · OG)*
- **E-LOAD-04** A memory bar or figure shows how much sample memory the project uses, and it grows or shrinks as samples are loaded or removed. *(likely · OG)*
- **E-LOAD-05** Up and down (and the encoder) move through the list that has focus; the other list keeps its position. Which key moves focus between the two lists (left/right arrows or an F-key) is not remembered. *(likely · OG; the focus key unsure)*
- **E-LOAD-06** Folders are entered and left without hunting: an entry for "up" or a back key, and the browser returns to the folder it came from with the cursor on the folder just left. *(likely · conv)*
- **E-LOAD-07** Folders are listed first, then files, each sorted by name; files the unit cannot load are hidden or marked. *(likely · conv)*
- **E-LOAD-08** Long names are shortened so the start of the name stays readable; two files that differ only at the end can still be told apart. *(unsure · conv)*
- **E-LOAD-09** The encoder scrolls long folders quickly when turned fast (acceleration), so a folder of hundreds of files is not a chore. *(likely · conv)*
- **E-LOAD-10** The target of a load is the instrument slot highlighted in the slot list; it is clearly highlighted before the load and is the same "current instrument" that the other instrument screens show. *(likely · OG)*
- **E-LOAD-11** One key (an F-key labelled as a load) puts the highlighted file into the target slot; afterwards the slot shows the file name and the pads play the new sample at once. *(likely · OG)*
- **E-LOAD-12** Loading into a slot that already holds a sample replaces it. An owner expects to notice what is being replaced (a confirmation, or at least a clear message); whether the official firmware asks first is not remembered. *(unsure · OG)*
- **E-LOAD-13** Whether a replace keeps the slot's instrument parameters (volume, filter, envelopes) or resets them is an open question; an owner expects it to be predictable and the same every time. *(unsure · OG)*
- **E-LOAD-14** Several files can be marked (Shift with the arrows) and loaded in one go into consecutive slots starting at the target slot; all marked files are highlighted before the load. *(likely · OG)*
- **E-LOAD-15** A multi-file load that runs past slot 48 stops at 48; it never wraps round to slot 1 and never overwrites slots outside the range it showed. *(likely · conv)*
- **E-LOAD-16** A file that does not fit in the free memory is refused with a clear message; nothing is half-loaded and the slot keeps what it had. *(likely · OG)*
- **E-LOAD-17** A long load shows progress; the screen does not freeze silently. *(likely · OG)*
- **E-LOAD-18** Common WAV files load: 16- and 24-bit, mono and stereo, 44.1 and 48 kHz. On the original Tracker a stereo file becomes mono when loaded. *(likely · OG for the mono summing; the accepted rates unsure)*
- **E-LOAD-19** A slot can be cleared from this screen (Delete or an F-key); it warns first, frees the memory, and the pattern cells that use the slot keep their instrument number. *(likely · OG for clearing; the warning likely · conv)*
- **E-LOAD-20** A slot can be copied to another slot (Copy, then paste with Shift + Copy on the target). *(unsure · OG)*
- **E-LOAD-21** An empty slot looks clearly empty, different from a slot whose sample failed to load. *(likely · conv)*
- **E-LOAD-22** Sample Load works while the sequencer plays; playback does not stop when a sample loads (a short glitch is tolerated). *(unsure · OG)*
- **E-LOAD-23** Leaving and coming back to the screen keeps the folder, the file cursor and the target slot. *(likely · conv)*
- **E-LOAD-24** The pads in this screen still make sound (the current instrument, or the highlighted file at the pad's pitch); a pad that does nothing here would feel broken. *(unsure · OG)*
- **E-LOAD-25** Sample Load addresses only the sample slots 1..48; MIDI instruments (49..64) are not load targets. *(likely · OG)*

## 2. File (projects)

- **E-FILE-01** The screen lists the projects on the card by name and marks the one that is open. *(likely · OG)*
- **E-FILE-02** The F-keys offer at least Open, Save, Save As, New and Export; Import (of a module file such as MOD) and Delete may be there as well. *(likely · OG for the first five; Import and Delete unsure)*
- **E-FILE-03** Save writes the open project under its current name without a question and confirms with a short message. *(likely · OG)*
- **E-FILE-04** Save As opens an on-screen keyboard for the name; the 48 pads type the characters laid out on screen as a 4 × 12 grid, the arrows or the encoder move a cursor over the keyboard, and F-keys confirm, cancel, delete a character and change case. *(likely · OG; the exact F-key set unsure)*
- **E-FILE-05** Save As with the name of an existing project warns before overwriting it. *(likely · conv)*
- **E-FILE-06** Open, New and Import, when the current project has unsaved changes, warn first and offer to save, discard or cancel. *(likely · conv; unsure that the official firmware offers all three)*
- **E-FILE-07** New gives empty patterns, an empty song and empty instrument slots, at a default tempo. *(likely · OG; the default tempo value unsure)*
- **E-FILE-08** Opening a project or making a new one stops playback first. *(likely · conv)*
- **E-FILE-09** Opening a project shows progress while its samples load. *(likely · OG)*
- **E-FILE-10** At power-on the unit opens the last project the user worked on. *(likely · OG)*
- **E-FILE-11** A project is self-contained on the card: its samples are copied into the project's own folder, so the project still works after the original sample files are moved or deleted. *(likely · OG)*
- **E-FILE-12** Export renders WAV files to an export folder named after the project; the choices include the song, the song as stems (one file per track), the pattern, and the pattern as stems. *(likely · OG; the exact list unsure)*
- **E-FILE-13** Export shows progress, can be cancelled, and leaves the project exactly as it was (it does not count as an unsaved change). *(likely · conv)*
- **E-FILE-14** Exporting again with the same name warns before overwriting earlier files, or writes new names. *(unsure · conv)*
- **E-FILE-15** Deleting a project warns first; the open project cannot be deleted from under itself. *(unsure · conv)*
- **E-FILE-16** Names have a length limit and a set of allowed characters; a forbidden or empty name is refused with a message, not silently changed. *(likely · conv)*
- **E-FILE-17** Some sign shows that the open project has unsaved changes. *(unsure · conv; not remembered in the official firmware)*
- **E-FILE-18** Whether edits survive a power cut without Save (a working copy) is an open question; an owner expects a save to be either complete or not done at all, never a broken project. *(unsure · OG)*
- **E-FILE-19** A module file (MOD) can be imported into a new project with its samples and patterns. *(unsure · OG)*

## 3. Sample Play / Wave (playback settings and slices)

- **E-PLAY-01** The screen shows the waveform of the current instrument's sample with its number and name; an empty instrument shows a clear message instead of a blank area. *(likely · OG)*
- **E-PLAY-02** A play-mode choice offers one-shot, forward loop, backward loop, ping-pong loop, slice, beat slice, granular and wavetable. *(likely · OG; the exact names and order unsure)*
- **E-PLAY-03** The markers are start, loop start, loop end and end; an F-key selects a marker, the encoder moves it, and the selected marker is highlighted. Loop markers appear only in the loop modes. *(likely · OG)*
- **E-PLAY-04** Markers keep their order: start ≤ loop start < loop end ≤ end. A marker pushed against its neighbour stops there; it never jumps past it. *(likely · conv)*
- **E-PLAY-05** Zoom is on an F-key with the encoder; it centres on the selected marker, and when zoomed in the encoder moves the marker in finer steps. *(likely · OG)*
- **E-PLAY-06** Marker positions are shown as numbers (a percentage or a time) as well as on the waveform. *(likely · OG)*
- **E-PLAY-07** The pads play the instrument chromatically with the current settings, and a playhead line runs over the waveform while it sounds. *(likely · OG)*
- **E-PLAY-08** Every change is heard at once, also on notes the pattern is playing. *(likely · conv)*
- **E-PLAY-09** In slice mode the slices are drawn as numbered lines or regions; pad N plays slice N (pads counted left to right, top row first). *(likely · OG)*
- **E-PLAY-10** The slice an edit acts on is the selected slice, chosen with an F-key and the encoder, and shown highlighted with its number; whether pressing a pad also selects its slice is not remembered. *(likely · OG; select-by-pad unsure)*
- **E-PLAY-11** Adjust moves the start of the selected slice with the encoder and cannot move it past its neighbours. *(likely · OG)*
- **E-PLAY-12** Add creates a new slice and selects it; at the slice limit (48, one per pad) Add is refused with a message. Where the new slice goes (the playhead, the middle of the selected slice) is not remembered. *(likely · OG for the limit; the position unsure)*
- **E-PLAY-13** Delete (or a Remove F-key) removes the selected slice; the selection moves to a neighbour, never to nothing. *(likely · OG)*
- **E-PLAY-14** Auto slice places slices on the transients of the sample. Because it replaces the slices already there, an owner expects a warning when slices exist. *(likely · OG for auto slice; the warning unsure)*
- **E-PLAY-15** Even slicing (N equal parts, N chosen with the encoder) is expected by owners coming from other samplers; whether the official firmware has it is not remembered. *(unsure · conv)*
- **E-PLAY-16** In the pattern, the note value of a slice-mode instrument chooses the slice in the same order as the pads, so a note entered from pad N plays slice N. *(likely · OG)*
- **E-PLAY-17** Changing the play mode away from slice and back keeps the slices; changing mode keeps the markers. *(likely · conv)*
- **E-PLAY-18** Slices, markers and the play mode are saved with the project and come back identical after reopening it. *(sure · conv)*
- **E-PLAY-19** Beat slice plays each slice cut at the start of the next note (for chopped loops); details of how it differs from slice are not remembered. *(unsure · OG)*
- **E-PLAY-20** Granular mode has a grain position, length and shape; the encoder sweeps the position and the sound follows live. *(likely · OG for the mode; the parameter set unsure)*
- **E-PLAY-21** Wavetable mode has a table position (and a window size); sweeping the position changes the timbre without clicks. *(likely · OG for the mode; the parameters unsure)*
- **E-PLAY-22** Copy, Insert and Delete do nothing outside slice mode here; no key on this screen destroys the sample itself (that is Sample Edit's job). *(likely · conv)*

## 4. Pattern (the step editor)

Layout and cursor:

- **E-PAT-01** Eight tracks, each with a note, an instrument and two FX columns (the second FX column started life as a volume column); the screen shows part of the tracks and scrolls sideways with the cursor. *(likely · OG; eight tracks sure for the original Tracker)*
- **E-PAT-02** Row numbers on the left; every fourth row (a beat) is shaded so the bar can be read at a glance. *(likely · OG)*
- **E-PAT-03** Empty cells show dashes; notes read like C-4 or C#4; instrument numbers have a fixed width; FX show a short name and a value. *(likely · OG)*
- **E-PAT-04** The arrows move the cursor one cell: up and down by row, left and right by column, then into the next track. The view scrolls so the cursor is always visible. *(sure · OG)*
- **E-PAT-05** Moving down from the last row returns to the first row (wrap). *(unsure · OG)*
- **E-PAT-06** Turned while not editing, the encoder moves the cursor up and down the rows, fast when turned fast. *(likely · OG)*
- **E-PAT-07** The cursor position (row, track, column) is kept when leaving the screen and coming back. *(likely · conv)*

Edit (record) mode:

- **E-PAT-08** `Rec` toggles edit mode, and edit mode is unmistakable on screen (a coloured cursor or frame) and on the `Rec` light. *(likely · OG)*
- **E-PAT-09** Outside edit mode the pads only play; they never write into the pattern. *(sure · OG)*
- **E-PAT-10** In edit mode with the transport stopped, a pad on the note column writes the note with the current instrument at the cursor, sounds it, and moves the cursor down by the Step value (Step 0 leaves it in place). *(likely · OG)*
- **E-PAT-11** In edit mode the encoder changes the value of the cell under the cursor: semitones on a note, the number on an instrument, the value on an FX. *(likely · OG)*
- **E-PAT-12** With the cursor on an instrument column in edit mode, the pads pick the instrument (pad N = instrument N). *(likely · OG)*
- **E-PAT-13** With the cursor on an FX column in edit mode, the pads pick the FX type, and the screen shows which pad is which FX while choosing. *(likely · OG)*
- **E-PAT-14** Writing a note over an existing step replaces the note and instrument and keeps the FX columns. *(likely · conv)*
- **E-PAT-15** A note-off (and a cut) can be entered; owners remember it reached by turning the encoder below the lowest note or by a key combination. *(unsure · OG)*
- **E-PAT-16** In edit mode `Delete` clears the cell under the cursor (or every selected cell); Shift + `Delete` removes the step and pulls the rows below up. *(likely · OG for Delete; Shift + Delete unsure)*
- **E-PAT-17** In edit mode `Insert` pushes the rows from the cursor down by one in that track (the last row falls off); what Shift + `Insert` does is not remembered. *(likely · OG for Insert; Shift + Insert unsure)*
- **E-PAT-18** Outside edit mode, `Delete` and `Insert` do not change the pattern, so a stray press cannot destroy work. *(unsure · OG; owners would welcome it)*

Play, live record and following the playhead:

- **E-PAT-19** `Play` loops the current pattern from row 1; pressing it again stops. The playing row is highlighted. *(likely · OG)*
- **E-PAT-20** While playing and not editing, the view follows the playhead ("watch"); after stop the cursor is in a predictable place (back where it was, or at the top). *(likely · OG for following; the stop position unsure)*
- **E-PAT-21** `Rec` during playback is live recording: pads write at the playing row, rounded to the nearest step, with the current instrument, and are heard as they are played. *(likely · OG)*
- **E-PAT-22** Live recording does not erase notes on rows the user did not play (overdub, not replace). *(likely · conv)*
- **E-PAT-23** Whether the arrows and the encoder still edit cells during live recording, or only move the view, is not remembered; an owner expects whatever it does to be shown clearly. *(unsure · OG)*
- **E-PAT-24** Shift + `Play` starts from the cursor row (or plays the song); one of these is expected, which one is not remembered. *(unsure · OG)*

Selection, copy and paste, tools:

- **E-PAT-25** Shift with the arrows grows a rectangular selection from the cursor; it may span columns and tracks, and the highlight covers exactly the cells an action will touch. *(likely · OG)*
- **E-PAT-26** A quick way exists to select a whole track or the whole pattern (a Shift combination or a repeated press). *(unsure · OG)*
- **E-PAT-27** `Copy` copies the selection (or the cell under the cursor when nothing is selected) and confirms with a short message. *(likely · OG)*
- **E-PAT-28** Shift + `Copy` pastes with the top-left corner of the copied block at the cursor; it overwrites without asking; it is clipped at the last row and the last track; it works between patterns. *(likely · OG for the gesture and anchoring; clipping likely · conv)*
- **E-PAT-29** Pasted columns land on columns of the same kind (notes on notes, FX on FX). *(unsure · conv)*
- **E-PAT-30** Pattern edits can be undone; the gesture is a Shift combination that is not remembered. *(likely · OG for undo existing; the gesture unsure)*
- **E-PAT-31** With a selection, the encoder in edit mode changes all selected values at once (for example transposes all selected notes). *(unsure · OG)*
- **E-PAT-32** Fill, Randomise, Invert and Transpose tools act on the selection (or the track when nothing is selected); Fill asks for the value and the step interval. *(likely · OG for the tools existing; the exact list unsure)*

F-keys and the pattern itself:

- **E-PAT-33** F-keys in this screen select Tempo, Pattern number, Length and Step; the encoder changes the selected one. *(likely · OG)*
- **E-PAT-34** The pattern length runs from 1 to 128 steps; shortening hides the rows past the end rather than deleting them. *(likely · OG for 1..128; hiding unsure)*
- **E-PAT-35** Changing the pattern number while playing switches at the end of the current pattern, not in the middle of a bar. *(unsure · OG)*
- **E-PAT-36** Track headers show the track number and mute state, and flash or meter when the track plays a note. *(likely · conv)*
- **E-PAT-37** The current instrument is visible on this screen, so the user knows what a pad press will write. *(likely · conv)*
- **E-PAT-38** The pads span four octaves (one row per octave); notes outside that range are reached with the encoder. *(likely · OG)*

## 5. Instrument (parameters, choosing an instrument)

- **E-INST-01** The screen shows the current instrument's number and name, and its parameters: volume, panning, tune, finetune, glide, filter type, cutoff, resonance and effect sends (reverb, and delay on later versions), plus drive or bit depth. *(likely · OG; the exact set unsure)*
- **E-INST-02** F-keys pick a parameter or a column of parameters, the arrows move between parameters, and the encoder changes the value. *(likely · OG)*
- **E-INST-03** Each value is shown as a number and a bar or knob; the units are plain (semitones, cents, percent, left/right pan). *(likely · conv)*
- **E-INST-04** Every change is heard at once on the pads and on notes the sequencer is playing, without clicks. *(sure · conv)*
- **E-INST-05** A filter set to off greys out cutoff and resonance rather than hiding them. *(unsure · conv)*
- **E-INST-06** A second page holds the envelopes (volume, filter, and the play-mode targets such as wavetable or granular position), each switchable to an LFO with shape and speed (speed synced to tempo). *(likely · OG)*
- **E-INST-07** The pads play the current instrument chromatically over four octaves. *(likely · OG)*
- **E-INST-08** There is one "current instrument" shared by Sample Load, Sample Play, Sample Edit and this screen; choosing it in one screen changes it in all. *(likely · OG)*
- **E-INST-09** Choosing an instrument is quick: a list or grid of 1..48 (sample) and 49..64 (MIDI), the pads light for slots that hold a sample, and pressing a pad picks that slot. The exact key that opens it is not remembered. *(likely · OG; the gesture unsure)*
- **E-INST-10** Choosing an instrument never changes the pattern. *(sure · conv)*
- **E-INST-11** A MIDI instrument (49..64) shows MIDI settings (channel and similar) instead of sample parameters. *(likely · OG; the set unsure)*
- **E-INST-12** An empty instrument shows that it is empty; its parameters may still be set ahead of a load. *(unsure · OG)*
- **E-INST-13** FX in the pattern (volume, panning, cutoff and similar) override a parameter for that note only; the stored instrument value does not change. *(likely · OG)*
- **E-INST-14** Glide applies when a new note arrives while the previous one still sounds. *(likely · conv)*
- **E-INST-15** The instrument can be renamed with the same on-screen keyboard as File. *(unsure · OG)*
- **E-INST-16** Instrument parameters are saved with the project, not in a separate file the user has to manage. *(likely · OG)*

## 6. Song (the arrangement)

- **E-SONG-01** The song is a list of slots played top to bottom; each slot holds a pattern number. *(likely · OG)*
- **E-SONG-02** The arrows (and the encoder) move through the slots; the encoder (or an F-key with the encoder) changes the pattern number of the selected slot. *(likely · OG)*
- **E-SONG-03** `Insert` adds a slot at the cursor and `Delete` removes the slot; removing a slot never deletes the pattern's contents. *(likely · OG for Insert/Delete; never deleting the pattern sure · conv)*
- **E-SONG-04** `Play` in this screen plays the song (from the selected slot or from the top), and the slot playing is highlighted and followed. *(likely · OG; the start slot unsure)*
- **E-SONG-05** At the end of the song playback loops to the first slot. *(likely · OG)*
- **E-SONG-06** Going to Pattern shows the pattern of the selected (or playing) slot. *(likely · OG)*
- **E-SONG-07** Editing a slot during playback takes effect when playback reaches it, without a stop or a glitch. *(likely · conv)*
- **E-SONG-08** Slots can be selected with Shift and the arrows, then copied and pasted with `Copy` and Shift + `Copy`. *(unsure · OG)*
- **E-SONG-09** A pattern can be duplicated to a new free pattern number from here (to make a variation without touching the original). *(unsure · OG)*
- **E-SONG-10** Clearing the whole song warns first. *(likely · conv)*
- **E-SONG-11** The song length (number of slots) is visible, and an empty song plays nothing rather than failing silently. *(unsure · conv)*

## 7. Sample Edit (destructive edits)

- **E-EDIT-01** The screen shows the current instrument's waveform with a range (start and end markers) moved with F-keys and the encoder, and a zoom. *(likely · OG)*
- **E-EDIT-02** A list of edits offers at least crop, normalise, reverse, fade in, fade out and amplify, plus processing effects (filter, bitcrusher, delay, chorus, compressor and similar). *(likely · OG; the exact list unsure)*
- **E-EDIT-03** An edit acts on the range between the markers; with the range at full length it acts on the whole sample. *(likely · OG)*
- **E-EDIT-04** An edit with settings can be previewed (heard) before it is applied; preview never changes the sample. *(likely · OG)*
- **E-EDIT-05** Apply changes the sample in memory and the waveform redraws at once; a long edit shows progress. *(likely · OG)*
- **E-EDIT-06** At least one level of undo exists for the last applied edit; how many levels is not remembered. *(likely · OG for one level)*
- **E-EDIT-07** The edit changes the project's copy of the sample, never the original file the user loaded from the card. *(likely · OG)*
- **E-EDIT-08** Crop frees memory and the memory figure updates; an edit that needs more memory than is free is refused with a message. *(likely · conv)*
- **E-EDIT-09** After a crop or cut, slices and loop markers stay inside the new sample (moved or reset); none points past the end. *(unsure · conv)*
- **E-EDIT-10** Normalise brings the peak to full scale; fades are smooth over the range; reverse is exact (applying it twice gives the original). *(likely · conv)*
- **E-EDIT-11** The pads (or `Play`) audition the sample or the range, so the result of an edit can be heard right away. *(unsure · OG)*
- **E-EDIT-12** Edits apply only to the current instrument; another instrument that loaded the same file keeps its own sound. *(unsure · OG)*

## 8. Sample Record (recording a sample)

- **E-SREC-01** The source can be chosen: line in, microphone, and the built-in FM radio. *(likely · OG)*
- **E-SREC-02** With the radio as source, the encoder tunes the frequency and a scan finds stations; the frequency is shown. *(likely · OG)*
- **E-SREC-03** An input gain setting and a level meter are visible before and during recording; clipping is shown. *(likely · OG)*
- **E-SREC-04** Monitoring can be switched on so the input is heard through the outputs. *(likely · OG)*
- **E-SREC-05** One key starts recording and one stops it (F-keys); the waveform draws live while recording and the time recorded is shown. *(likely · OG)*
- **E-SREC-06** The maximum length is limited by free memory and is shown; recording stops cleanly at the limit with a message. *(likely · conv)*
- **E-SREC-07** After recording, the take can be heard, trimmed (start and end) and then saved: a name is typed, the file is written to the card as WAV and loaded into an instrument slot. *(likely · OG)*
- **E-SREC-08** Saving a take over an existing file name or into an occupied slot warns first. *(likely · conv)*
- **E-SREC-09** A take can be discarded and a new one started without leaving the screen; discarding warns if the take was not saved. *(unsure · conv)*
- **E-SREC-10** Recording is mono. *(likely · OG)*
- **E-SREC-11** Recording that starts when the input passes a threshold, and resampling the unit's own output, are open questions for the official firmware. *(unsure · OG)*

## 9. Mixer

- **E-MIX-01** The screen shows a level and a live meter for each of the eight tracks, plus the master. *(likely · OG)*
- **E-MIX-02** F1..F8 relate to tracks 1..8: a press mutes or unmutes the track, and the mute shows on the screen and in the Pattern track headers. *(likely · OG)*
- **E-MIX-03** Solo exists (a Shift combination with the track's F-key); whether it is exclusive or adds to other solos is not remembered. *(unsure · OG)*
- **E-MIX-04** A track's level is changed with the encoder after selecting the track. *(likely · OG; the selection gesture unsure)*
- **E-MIX-05** Mute, solo and level act at once and without clicks, also while playing. *(sure · conv)*
- **E-MIX-06** Master settings (volume, reverb, limiter, bit depth) are reachable from here or from a neighbouring master page. *(unsure · OG)*
- **E-MIX-07** Whether mutes are saved with the project or reset on reopening is an open question; an owner expects the same rule every time. *(unsure · OG)*

## 10. Perform (performance effects and modes)

- **E-PERF-01** In Perform the pads stop being a keyboard and become effect controls: each column of pads is one effect (tune, filter, volume, sample position, repeat or stutter, tempo and similar) and the rows are its amounts. *(likely · OG)*
- **E-PERF-02** An effect is active only while its pad is held; releasing returns the sound to normal. *(likely · OG)*
- **E-PERF-03** F1..F8 choose which tracks the effects act on (or mute tracks); their state shows on screen. *(likely · OG; which of the two unsure)*
- **E-PERF-04** The effect assigned to a column can be changed on screen (F-key or arrows with the encoder) and the assignment is kept with the project. *(unsure · OG)*
- **E-PERF-05** Time-based effects (repeat, stutter) lock to the tempo. *(likely · conv)*
- **E-PERF-06** Perform never writes into the patterns; whatever it does is gone when the pads are released or the screen is left. *(likely · OG)*
- **E-PERF-07** Patterns can be switched per track from this screen during playback, the switch landing at the end of the pattern. *(unsure · OG)*
- **E-PERF-08** Leaving Perform while a pad is held does not leave an effect stuck on. *(sure · conv)*

## 11. Config (settings)

- **E-CFG-01** Settings are grouped in categories (general, MIDI, interface, firmware and similar); F-keys or the arrows move between them, the encoder changes a value. *(likely · OG)*
- **E-CFG-02** A changed setting applies at once and survives a power cycle, independently of the open project. *(likely · OG)*
- **E-CFG-03** MIDI settings cover clock in and out (sync), transport in and out, note input channel, and DIN versus USB. *(likely · OG)*
- **E-CFG-04** The firmware version is shown. *(sure · conv)*
- **E-CFG-05** A firmware update is started from here: it lists the update files found in the card's firmware folder, the user picks one, and it asks for confirmation before updating. *(likely · OG)*
- **E-CFG-06** Display and pad light brightness can be set. *(unsure · OG)*
- **E-CFG-07** A factory reset of the settings, if offered, warns first and never touches projects or samples. *(unsure · conv)*

## 12. Global keys and transport

- **E-GLOB-01** A screen button opens its screen at once from any other screen, and playback does not stop. *(sure · OG)*
- **E-GLOB-02** The light of the current screen's button is on. *(likely · OG)*
- **E-GLOB-03** Some screens are reached with Shift + a screen button; which ones is not remembered. *(unsure · OG)*
- **E-GLOB-04** `Play` starts and stops the sequencer from any screen, except where the screen gives it its own meaning (Sample Load's audition, see E-LOAD-01). *(likely · OG)*
- **E-GLOB-05** Stopping and starting again begins from the start of the pattern (or the song position the screen implies), never from a random place. *(likely · OG)*
- **E-GLOB-06** `Rec` and `Play` together start live recording; `Rec` alone with the transport stopped is step editing (see Pattern). *(likely · OG)*
- **E-GLOB-07** The pads play the current instrument chromatically in every screen that does not give them another job (on-screen keyboard, Perform, picking an instrument, an FX or a slice). *(likely · OG)*
- **E-GLOB-08** A pad lights while held; a looping or sustained sound stops on release, a one-shot plays to its end. *(likely · OG)*
- **E-GLOB-09** The labels at the bottom of the screen sit above the F-key they belong to; an F-key without a label does nothing, and a selected function is highlighted. *(likely · OG)*
- **E-GLOB-10** An F-key selects its parameter and the encoder then changes it; pressing the same F-key again either toggles or opens a list, consistently across screens. *(likely · OG)*
- **E-GLOB-11** The encoder accelerates: a slow turn moves one step, a fast turn moves many. *(likely · conv)*
- **E-GLOB-12** `Copy` and Shift + `Copy` mean copy and paste everywhere they work; `Insert` and `Delete` mean insert and remove everywhere they work. A key with no meaning on a screen does nothing. *(likely · conv)*
- **E-GLOB-13** Every confirmation popup puts its choices on F-keys with clear labels; the harmless choice is easy to reach, and a screen button or the same key leaves the popup without doing anything. *(likely · OG)*
- **E-GLOB-14** Short messages ("saved", "copied", "not enough memory") appear in a fixed place and go away on their own. *(likely · OG)*
- **E-GLOB-15** Notes coming in over MIDI play the current instrument (or the one mapped to their channel) whichever screen is open. *(likely · OG)*
- **E-GLOB-16** When an external MIDI clock drives the tempo, `Play` and the tempo control follow it and say so, rather than fighting it. *(unsure · conv)*
- **E-GLOB-17** Every change to sound (instrument, mixer, sample) is heard live while the sequencer plays; no screen needs a stop to take effect, except loading a project. *(likely · conv)*
- **E-GLOB-18** No key has a hidden destructive effect on a screen where its label is not shown. *(likely · conv)*

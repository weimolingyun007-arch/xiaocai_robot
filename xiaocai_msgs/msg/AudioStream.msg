builtin_interfaces/Time timestamp
# Interleaved signed 16-bit little-endian PCM; no file or FIFO paths.
# Action values are defined by common::AudioStreamAction.
uint8 action
uint64 stream_id
uint64 sequence
uint32 sample_rate 48000
uint8 channels 2
uint8[] data

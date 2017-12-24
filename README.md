# Async disk shredding

I haven't done comprehensive benchmarks, but this setup increased my write
throughput from ~30MB/s using dd if=/dev/urandom to ~100MB/s when cleaning off
a bunch of old hard drives. Different queue sizes and buffer sizes may be
required for different systems for max throughput, but the config here should
be fast most of the time.

This currently uses BSD ioctls to retrieve disk size. Conversion to linux should
be easy, but I haven't bothered yet.

ASSERT(___data_high - LOAD_ADDR <= 0x20000,
       "EMUDORE expanded image exceeds 128 KiB; leave RAM for CE libraries and the startup loader")
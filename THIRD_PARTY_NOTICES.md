# Third-party notices

This port builds the third-party code listed first, and builds on the work credited after it. Where it comes
from, and which files were taken, is in `third_party/README.md`.

## Code in this repository

### GekkoNet (BSD 2-Clause)

Copyright (c) 2024-2026, Jamie Meyer. Upstream: https://github.com/HeatXD/GekkoNet. Used for the lockstep input
exchange of online play (without its ASIO adapter: the port's own transport carries the packets). Licence file:
`third_party/GekkoNet/LICENSE`, reproduced here.

```
BSD 2-Clause License

Copyright (c) 2024-2026, Jamie Meyer

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zpp_bits (MIT), inside GekkoNet

Copyright (c) 2021 Eyal Z. Upstream: https://github.com/eyalz800/zpp_bits. GekkoNet serialises its packets with it;
the header is taken as GekkoNet ships it (`third_party/GekkoNet/thirdparty/zpp/`). Licence file:
`third_party/GekkoNet/thirdparty/zpp/LICENSE`, reproduced here.

```
MIT License

Copyright (c) 2021 Eyal Z

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### miniupnpc (BSD 3-Clause)

Copyright (c) 2005-2025, Thomas Bernard. Upstream: https://github.com/miniupnp/miniupnp. Used to ask the router
to open the host's UDP port (UPnP). Licence file: `third_party/miniupnpc/LICENSE`, reproduced here.

```
BSD 3-Clause License

Copyright (c) 2005-2025, Thomas BERNARD
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### DOA2U recomp: Wi-Fi media streaming mode (MIT)

DOA2U recomp by Parker Allan, parkerallan/doa2u-recomp, MIT. Upstream: https://github.com/parkerallan/doa2u-recomp.
`src/net/np_transport.cpp` (`Transport::Impl::WifiOn` / `WifiOff`) is ported from `doa2u/src/nic/nic_net.c`
(`wlan_no_background_scans`, commit `361dccf`): media streaming mode on every connected Wi-Fi interface while the
transport runs, so Windows does not scan for other networks and hold outgoing frames. The DOA2U project states that its
code is MIT ("Project code is MIT", its README, section License) and ships no separate licence file; the MIT
permission notice is reproduced here with the author's name.

```
MIT License

Copyright (c) 2026 Parker Allan

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Credits

- Tekken 6 online, by Ekspym and mahmoud-mess: the audio and XMA lockstep of the SDK fork this port runs on
  is theirs. Used with permission (netplay spec, section 3.1).
- The DOA2U netplay (branch `netplay-release` of the DOA2U recompilation project): the lobby, join codes, hand-off
  files and connection layer of this port are ported from it, and GekkoNet and miniupnpc were copied from its
  `third_party/` folder.
- GGPO: no GGPO code is taken. GekkoNet is inspired by it and carries no GGPO code, so no GGPO notice is needed.

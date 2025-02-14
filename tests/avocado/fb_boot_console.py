# Test that fb machine types boot successfully.
#
# Copyright (c) Meta Platforms, Inc. and affiliates. (http://www.meta.com)
#
# Author:
#   Peter Delevoryas <pdel@fb.com>
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

from avocado_qemu import (QemuSystemTest, wait_for_console_pattern,
                          exec_command_and_wait_for_pattern)

class BootTests(QemuSystemTest):
    timeout = 500

    def test_fby35_bmc(self):
        """
        :avocado: tags=arch:arm
        :avocado: tags=machine:fby35-bmc
        """
        image_url = 'https://github.com/facebook/openbmc/releases/download/openbmc-e2294ff5d31d/fby35.mtd'
        image_hash = '0a3635646f38373e318811be1ec3743540cc456aafe87234655081684b03b713'
        image_path = self.fetch_asset(image_url, asset_hash=image_hash, algorithm='sha256')

        self.vm.set_console()
        self.vm.add_args('-drive', f'file={image_path},format=raw,if=mtd',
                         '-drive', f'file={image_path},format=raw,if=mtd', '-netdev',
                         'user,id=nic,mfr-id=0x8119,oob-eth-addr=de:ad:be:ef:ca:fe,hostfwd=::2222-:22',
                         '-net', 'nic,model=ftgmac100,netdev=nic')
        self.vm.launch()
        wait_for_console_pattern(self, 'OpenBMC Release fby35-e2294ff5d3', vm=self.vm)

        # FIXME: For some reason the login prompt doesn't appear, but if we can get it to work, I'd
        # like to verify that the MAC gets set properly.
        #wait_for_console_pattern(self, 'bmc-oob. login:', vm=self.vm)
        #exec_command_and_wait_for_pattern(self, 'root', 'Password:')
        #exec_command_and_wait_for_pattern(self, '0penBmc', 'root@bmc-oob:~#')
        #exec_command_and_wait_for_pattern(self, 'ifconfig', 'HWaddr DE:AD:BE:EF:CA:FE')

    def do_test_bic(self, kernel_url, kernel_hash):
        kernel_path = self.fetch_asset(kernel_url, asset_hash=kernel_hash, algorithm='sha256')

        self.vm.set_console()
        self.vm.add_args('-kernel', kernel_path)
        self.vm.launch()
        wait_for_console_pattern(self, 'uart:~$', vm=self.vm)

    def test_oby35_cl(self):
        """
        :avocado: tags=arch:arm
        :avocado: tags=machine:oby35-cl
        """
        kernel_url = 'https://github.com/peterdelevoryas/OpenBIC/releases/download/oby35-cl-2022.13.01/Y35BCL.elf'
        kernel_hash = '017edb61244c609de7b5cd8c19258e961aecae902aba8303e6d4351868184ab6'
        self.do_test_bic(kernel_url, kernel_hash)

    def test_oby35_bb(self):
        """
        :avocado: tags=arch:arm
        :avocado: tags=machine:oby35-bb
        """
        kernel_url = 'https://github.com/peterdelevoryas/OpenBIC/releases/download/oby35-bb-2022.13.01/Y35BBB.elf'
        kernel_hash = '9a484650732ec9ab2202d9bab906a3babb530def46797c01c068e725e2d89ac4'
        self.do_test_bic(kernel_url, kernel_hash)

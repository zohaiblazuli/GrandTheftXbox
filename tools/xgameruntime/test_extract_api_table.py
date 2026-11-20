"""Tests for the XGameRuntime dispatch-table extractor (synthetic dumpbin text)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import extract_api_table as ex  # noqa: E402

ASM = """
XUserGetId:
  0000000000000027: 48 8D 0D 00 00 00  lea         rcx,[_GUID_01acd177_91f9_4763_a38e_ccbb55ce32e0]
  000000000000002E: 48 8B D1           mov         rdx,rcx
  0000000000000031: E8 00 00 00 00     call        QueryApiImpl
  0000000000000041: 48 8B 01           mov         rax,qword ptr [rcx]
  000000000000004A: 48 8B 40 58        mov         rax,qword ptr [rax+58h]
  000000000000005D: 48 8B 40 10        mov         rax,qword ptr [rax+10h]

XSystemGetConsoleId:
  0000000000000010: 48 8D 15 00 00 00  lea         rdx,[_GUID_1861cf2e_e18b_4834_a9f5_b4a4e6efb4cf]
  0000000000000017: 48 8D 0D 00 00 00  lea         rcx,[_GUID_e349bd1a_fc20_4e40_b99c_4178cc6b409f]
  0000000000000020: E8 00 00 00 00     call        QueryApiImpl
  0000000000000030: 48 8B 40 18        mov         rax,qword ptr [rax+18h]

XSystemGetDeviceType:
  0000000000000024: FF 15 00 00 00 00  call        qword ptr [__imp_RegOpenKeyW]

?Helper@@YAJXZ (long __cdecl Helper(void)):
  0000000000000000: E8 00 00 00 00     call        QueryApiImpl
  0000000000000010: 48 8B 40 18        mov         rax,qword ptr [rax+18h]
"""


class ExtractTests(unittest.TestCase):
    def test_same_guid(self):
        t = ex.parse_disasm(ASM)
        self.assertEqual(t["XUserGetId"], {"guid": "01ACD177-91F9-4763-A38E-CCBB55CE32E0",
                                           "provider": "01ACD177-91F9-4763-A38E-CCBB55CE32E0",
                                           "slot": 11})

    def test_split_provider_and_iid(self):
        t = ex.parse_disasm(ASM)
        self.assertEqual(t["XSystemGetConsoleId"]["guid"], "1861CF2E-E18B-4834-A9F5-B4A4E6EFB4CF")
        self.assertEqual(t["XSystemGetConsoleId"]["provider"], "E349BD1A-FC20-4E40-B99C-4178CC6B409F")
        self.assertEqual(t["XSystemGetConsoleId"]["slot"], 3)

    def test_local_and_non_api_functions_skipped(self):
        t = ex.parse_disasm(ASM)
        self.assertNotIn("XSystemGetDeviceType", t)  # implemented inside the static lib
        self.assertEqual(len(t), 2)

    def test_ret_kind_and_inc(self):
        self.assertEqual(ex.ret_kind("HRESULT"), "Hr")
        self.assertEqual(ex.ret_kind("void"), "Void")
        self.assertEqual(ex.ret_kind("XSystemAnalyticsInfo"), "Struct")
        self.assertEqual(ex.ret_kind("bool"), "Zero")
        table = {"source": "s", "gdk_dir": "d", "api_count": 1, "interface_count": 1,
                 "interfaces": [{"guid": "G", "max_slot": 11}],
                 "apis": [{"name": "XUserGetId", "guid": "G", "slot": 11, "ret": "HRESULT"}]}
        inc = ex.render_inc(table)
        self.assertIn('GTX_XGR_IFACE(0, "G", 12)', inc)
        self.assertIn("GTX_XGR_API(XUserGetId, 0, 11, Hr)", inc)


if __name__ == "__main__":
    unittest.main()

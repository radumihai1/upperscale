@echo off
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
 "$c=@'
 using System;
 using System.Runtime.InteropServices;
 [StructLayout(LayoutKind.Sequential)] public struct LUID { public int LowPart; public int HighPart; }
 [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
 public struct DXGI_ADAPTER_DESC1 {
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string Description;
  public uint VendorId, DeviceId, SubSysId, Revision;
  public UIntPtr DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
  public LUID AdapterLuid;
  public uint Flags;
 }
 [ComImport, Guid(""770aae78-f26f-4dba-a829-253c83d1b387""), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
 interface IDXGIFactory1 { [PreserveSig] int EnumAdapters1(uint i, out IDXGIAdapter1 a); }
 [ComImport, Guid(""29038f61-3839-4626-91fd-086879011a05""), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
 interface IDXGIAdapter1 { [PreserveSig] int GetDesc1(out DXGI_ADAPTER_DESC1 d); }
 public static class DX {
  [DllImport(""dxgi.dll"")] public static extern int CreateDXGIFactory1(ref Guid riid, out IntPtr ppFactory);
  static Guid IID_IDXGIFactory1=new Guid(""770aae78-f26f-4dba-a829-253c83d1b387"");
  public static void List(){
    IntPtr pFact; int hr=CreateDXGIFactory1(ref IID_IDXGIFactory1, out pFact);
    if(hr<0){ Console.WriteLine(""CreateDXGIFactory1 failed: 0x{0:X}"", hr); return; }
    var factory=(IDXGIFactory1)Marshal.GetObjectForIUnknown(pFact);
    uint i=0; for(;;){
      IDXGIAdapter1 ad; int hr2=factory.EnumAdapters1(i, out ad);
      if(hr2<0) break;
      DXGI_ADAPTER_DESC1 desc; ad.GetDesc1(out desc);
      ulong low=unchecked((uint)desc.AdapterLuid.LowPart);
      ulong high=unchecked((uint)desc.AdapterLuid.HighPart);
      ulong full=(high<<32)|low;
      Console.WriteLine(""GPU{0}: {1}"", i, desc.Description);
      Console.WriteLine(""  LUID low=0x{0:X}  high=0x{1:X}  full=0x{2:X}"", low, high, full);
      Console.WriteLine(""  VendorId=0x{0:X}  DeviceId=0x{1:X}  VRAM={2} MB"", desc.VendorId, desc.DeviceId, (long)desc.DedicatedVideoMemory/1024/1024);
      i++;
    }
  }
 }
'@; Add-Type -TypeDefinition $c; [DX]::List()"
pause

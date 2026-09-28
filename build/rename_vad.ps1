Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class VadNames {
  [StructLayout(LayoutKind.Sequential)]
  public struct PROPERTYKEY {
    public Guid fmtid;
    public uint pid;
  }

  [StructLayout(LayoutKind.Explicit)]
  public struct PROPVARIANT {
    [FieldOffset(0)] public ushort vt;
    [FieldOffset(8)] public IntPtr pwszVal;
  }

  [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IPropertyStore {
    int GetCount(out uint cProps);
    int GetAt(uint iProp, out PROPERTYKEY pkey);
    int GetValue(ref PROPERTYKEY key, out PROPVARIANT pv);
    int SetValue(ref PROPERTYKEY key, ref PROPVARIANT pv);
    int Commit();
  }

  [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDevice {
    int Activate(ref Guid iid, uint ctx, IntPtr param, out IntPtr iface);
    int OpenPropertyStore(uint stgm, out IPropertyStore store);
    int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
    int GetState(out uint state);
  }

  [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDeviceCollection {
    int GetCount(out uint count);
    int Item(uint index, out IMMDevice device);
  }

  [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  interface IMMDeviceEnumerator {
    int EnumAudioEndpoints(int flow, uint stateMask, out IMMDeviceCollection devices);
    int GetDefaultAudioEndpoint(int flow, int role, out IMMDevice device);
    int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
    int RegisterEndpointNotificationCallback(IntPtr client);
    int UnregisterEndpointNotificationCallback(IntPtr client);
  }

  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
  class MMDeviceEnumeratorCom { }

  [DllImport("ole32.dll")] static extern int PropVariantClear(ref PROPVARIANT pvar);

  static PROPERTYKEY Key() {
    return new PROPERTYKEY {
      fmtid = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"),
      pid = 14
    };
  }

  public static string Read(int flow) {
    var lines = new System.Text.StringBuilder();
    var en = (IMMDeviceEnumerator)new MMDeviceEnumeratorCom();
    IMMDeviceCollection col;
    en.EnumAudioEndpoints(flow, 1, out col);
    uint count;
    col.GetCount(out count);
    for (uint i = 0; i < count; i++) {
      IMMDevice dev;
      col.Item(i, out dev);
      string id;
      dev.GetId(out id);
      IPropertyStore store;
      dev.OpenPropertyStore(0, out store);
      PROPVARIANT value;
      PROPERTYKEY key = Key();
      store.GetValue(ref key, out value);
      string name = value.vt == 31 && value.pwszVal != IntPtr.Zero ? Marshal.PtrToStringUni(value.pwszVal) : "";
      PropVariantClear(ref value);
      lines.AppendLine(flow + "|" + name + "|" + id);
    }
    return lines.ToString();
  }

  public static string Rename(int flow, string fromName, string toName) {
    var en = (IMMDeviceEnumerator)new MMDeviceEnumeratorCom();
    IMMDeviceCollection col;
    en.EnumAudioEndpoints(flow, 1, out col);
    uint count;
    col.GetCount(out count);
    for (uint i = 0; i < count; i++) {
      IMMDevice dev;
      col.Item(i, out dev);
      IPropertyStore store;
      int hrOpen = dev.OpenPropertyStore(2, out store);
      if (hrOpen != 0 || store == null) return "open " + hrOpen;
      PROPVARIANT value;
      PROPERTYKEY key = Key();
      store.GetValue(ref key, out value);
      string name = value.vt == 31 && value.pwszVal != IntPtr.Zero ? Marshal.PtrToStringUni(value.pwszVal) : "";
      PropVariantClear(ref value);
      if (name != fromName) continue;
      var set = new PROPVARIANT();
      set.vt = 31;
      set.pwszVal = Marshal.StringToCoTaskMemUni(toName);
      int hr = store.SetValue(ref key, ref set);
      int commit = hr == 0 ? store.Commit() : 0;
      PropVariantClear(ref set);
      return "set " + hr + " commit " + commit + " " + fromName + " -> " + toName;
    }
    return "missing " + fromName;
  }
}
"@

$map = @(
  @(0, 'Speakers 01 (ASIOVADPRO Driver)', 'AnyMix Game'),
  @(0, 'Speakers 02 (ASIOVADPRO Driver)', 'AnyMix Chat'),
  @(0, 'Speakers 03 (ASIOVADPRO Driver)', 'AnyMix Music'),
  @(0, 'Speakers 04 (ASIOVADPRO Driver)', 'AnyMix Aux'),
  @(1, 'Mix 01 (ASIOVADPRO Driver)', 'AnyMix B1'),
  @(1, 'Mix 02 (ASIOVADPRO Driver)', 'AnyMix B2'),
  @(1, 'Mix 03 (ASIOVADPRO Driver)', 'AnyMix B3'),
  @(1, 'Mix 04 (ASIOVADPRO Driver)', 'AnyMix B4')
)

Write-Output '---- before render ----'
[VadNames]::Read(0) | Write-Output
Write-Output '---- before capture ----'
[VadNames]::Read(1) | Write-Output
Write-Output '---- rename ----'
foreach ($row in $map) {
  [VadNames]::Rename([int]$row[0], [string]$row[1], [string]$row[2])
}

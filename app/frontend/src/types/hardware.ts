// Mirrors core/hardware/HardwareInfo.h. `availableEncoders` is best-effort and may be
// empty -- never assume NVENC/QSV/AMF availability (spec section 22).
export type GpuVendor = "NVIDIA" | "AMD" | "INTEL" | "UNKNOWN";

export interface HardwareInfo {
  cpu: { name: string; logicalCores: number };
  gpus: Array<{ vendor: GpuVendor; name: string }>;
  availableEncoders: string[];
}

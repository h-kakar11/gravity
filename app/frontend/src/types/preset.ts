// Mirrors core/settings/PresetStore.h's Preset struct.
export type PresetKind = "DOWNLOAD" | "CONVERSION" | "COMPRESSION";

export interface Preset {
  id: string;
  name: string;
  kind: PresetKind;
  // A DownloadJobParams-shaped or MediaProcessingOptions-shaped object depending on `kind`
  // -- opaque to the store and to this type, same as on the C++ side.
  options: Record<string, unknown>;
}

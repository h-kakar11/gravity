import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it } from "vitest";
import { NavigationProvider, useNavigation } from "./NavigationContext";

function ScreenKindProbe() {
  const { screen: current } = useNavigation();
  return <span data-testid="screen-kind">{current.kind}</span>;
}

function NavigateButtons() {
  const { navigate } = useNavigation();
  return (
    <>
      <button onClick={() => navigate({ kind: "queue" })}>go queue</button>
      <button onClick={() => navigate({ kind: "convert", mode: "compress", prefillFilePath: "C:\\a.mov" })}>
        go convert
      </button>
    </>
  );
}

describe("NavigationContext", () => {
  it("starts on the home screen", () => {
    render(
      <NavigationProvider>
        <ScreenKindProbe />
      </NavigationProvider>,
    );
    expect(screen.getByTestId("screen-kind").textContent).toBe("home");
  });

  it("navigate() switches the current screen", () => {
    render(
      <NavigationProvider>
        <ScreenKindProbe />
        <NavigateButtons />
      </NavigationProvider>,
    );

    fireEvent.click(screen.getByText("go queue"));
    expect(screen.getByTestId("screen-kind").textContent).toBe("queue");
  });

  it("preserves the extra fields on the navigated-to screen (not just `kind`)", () => {
    let capturedScreen: unknown;
    function Capture() {
      const { screen: current } = useNavigation();
      capturedScreen = current;
      return null;
    }

    render(
      <NavigationProvider>
        <Capture />
        <NavigateButtons />
      </NavigationProvider>,
    );

    fireEvent.click(screen.getByText("go convert"));
    expect(capturedScreen).toEqual({ kind: "convert", mode: "compress", prefillFilePath: "C:\\a.mov" });
  });

  it("useNavigation() throws when used outside a NavigationProvider", () => {
    function Bare() {
      useNavigation();
      return null;
    }
    expect(() => render(<Bare />)).toThrow(/must be called within a NavigationProvider/);
  });
});

import argparse
from .case import capture_metadata

def main():
    parser = argparse.ArgumentParser(description="Capture privacy-preserving XBE replay metadata")
    parser.add_argument("--xbe", required=True)
    parser.add_argument("--func", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    capture_metadata(args.xbe, args.func, args.state, args.out)

if __name__ == "__main__":
    main()

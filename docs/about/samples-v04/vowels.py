def count(t: str) -> int:
    n = 0
    for c in t:
        if c in ("a", "e", "i", "o", "u"):
            n += 1
    return n


def main() -> None:
    print(count("the quick brown fox"))


main()

def main() -> None:
    xs = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    total = 0
    for v in xs:
        if v % 2 == 0:
            total += v * v
    print(total)


main()

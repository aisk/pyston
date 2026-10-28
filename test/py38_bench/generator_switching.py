def g(n):
    for i in range(n):
        yield n

def run():
    return len(list(g(5000000)))

if __name__ == "__main__":
    run()

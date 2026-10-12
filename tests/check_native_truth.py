#!/usr/bin/env python3
"""Execute the supplied truth table through native source lowering."""
import sys
import tempfile
from pathlib import Path
from check_verification import run, failure

VALUES = [1, .5, 0, -.5, -1]
AND = [[1,.5,0,-.5,-1],[.5,.5,0,-.5,-1],[0,0,0,-.5,-1],[-.5,-.5,-.5,-.5,-1],[-1,-1,-1,-1,-1]]
OR = [[1,1,1,1,1],[1,.5,.5,.5,.5],[1,.5,0,0,0],[1,.5,0,-.5,-.5],[1,.5,0,-.5,-1]]
IMPLIES = [[1,.5,0,-.5,-1],[1,.5,0,-.5,-.5],[1,.5,0,0,0],[1,.5,.5,.5,.5],[1,1,1,1,1]]
XOR = [[-1,-.5,0,.5,1],[-.5,-.5,0,.5,.5],[0,0,0,0,0],[.5,.5,0,-.5,-.5],[1,.5,0,-.5,-1]]
DIRECTION = [[0,-1,-1,-1,-1],[1,0,-1,-1,-1],[1,1,0,-1,-1],[1,1,1,0,-1],[1,1,1,1,0]]


def literal(number):
    return repr(float(number))


def execute(compiler, work, name, source, expected=b"0"):
    path, output = work / f"{name}.bpp", work / f"{name}.exe"
    path.write_text(source)
    for flags in ([], ["--no-opt"]):
        result = run([compiler, *flags, "--print-result", "-o", output, path], timeout=60)
        if result.returncode:
            failure(work, name + "-compile", result, source.encode())
        result = run([output])
        if result.returncode or result.stdout.strip() != expected:
            failure(work, name, result, source.encode())


def main():
    compiler = Path(sys.argv[1]).resolve()
    checks = [("NOT(a)", "na"), ("!b", "nb"), ("AND(a,c)", "ac"),
              ("AND(b,c)", "bc"), ("OR(a,c)", "ao"), ("OR(b,c)", "bo"),
              ("IMPLIES(a,c)", "ai"), ("IMPLIES(b,c)", "bi"),
              ("XOR(a,c)", "ax"), ("XOR(b,c)", "bx"),
              ("a => b", "direction"), ("velocity(a,b,2)", "rate")]
    parameters = ["a", "b", "c"] + [name for _, name in checks]
    source = "define proc probe(" + ",".join(parameters) + "){"
    for code, (expression, expected) in enumerate(checks, 1):
        source += f"if(({expression})!={expected}){{return {code};}}"
    source += "return 0;}"
    rows = []
    for a in range(5):
        for b in range(5):
            for c in range(5):
                expected = [VALUES[4-a], VALUES[4-b], AND[a][c], AND[b][c],
                            OR[a][c], OR[b][c], IMPLIES[a][c], IMPLIES[b][c],
                            XOR[a][c], XOR[b][c], DIRECTION[a][b],
                            (VALUES[b]-VALUES[a])/2]
                row = "failed=probe(" + ",".join(map(literal,
                    [VALUES[a], VALUES[b], VALUES[c], *expected])) + ");"
                row += f"if(failed>0){{return {(a*25+b*5+c+1)*100}+failed;}}"
                rows.append(row)
    with tempfile.TemporaryDirectory(prefix="bpp-native-truth-") as directory:
        work = Path(directory)
        # Keep each oracle batch within the same per-process CPU/output bounds
        # under instrumented GCC as under Clang. Stress has separate fixtures.
        for begin in range(0,len(rows),25):
            batch=source+"define proc main(){new byte failed=0;"+"".join(rows[begin:begin+25])+"return failed;}"
            execute(compiler, work, f"table-{begin//25}", batch)
        execute(compiler, work, "threshold", """
        define proc main(){
            if(velocity(-1.0,1.0,1)>=1.0){}else{return 1;}
            if(velocity(-1.0,1.0,4)>=1.0){return 2;}
            if(velocity(-1.0,1.0,4)>=0.5){}else{return 3;}
            new state current=-0.25;
            new byte iterations=0;
            while(velocity(current,0.25,1)>=0.5){iterations=iterations+1;current=0.25;}
            if(iterations!=1){return 4;}
            return 0;
        }""")
        (work / "errors-library.bpp").write_text("define proc fail(n){return 1/n;}\n")
        execute(compiler, work, "imported-error", 'import "errors-library.bpp";\n'
                'define proc main(){new state u=fail(0);if(error_line(u)!=1){return 1;}'
                'if(error_file(u)!=2){return 2;}return 0;}')
        execute(compiler, work, "memory", """define proc main(){
            new ptr p=malloc 2;if(!p!=false){return 10;}p[0]=42;new ptr view=p+1;view[-1]=43;
            if(p[0]!=43){return 1;}if(p!=(p+0)){return 2;}
            new byte u=p[1];if(faults(u)!=4){return 3;}
            new state outside=p[2];if(faults(outside)!=4){return 4;}
            p=remalloc p 4;if(p[0]!=43){return 5;}
            new state stale=view[0];if(faults(stale)!=4){return 6;}
            free p;new state dead=p[0];if(faults(dead)!=4){return 7;}
            new byte deleted=1;delete deleted;deleted=7;
            if(faults(deleted)!=4){return 8;}return 0;
        }""")
        execute(compiler, work, "error", """define proc divide(n){return 1/n;}
        define proc main(){new state u=divide(0);
            if(u!=0.0){return 1;}if(faults(u)!=4){return 2;}
            if(error_line(u)!=1){return 3;}
            new state propagated=OR(u,1.0);
            if(propagated!=1.0){return 4;}if(faults(propagated)!=4){return 5;}
            if(NOT(u)!=0.0){return 6;}
            new state invalid=velocity(-1.0,1.0,0);
            if(invalid!=0.0){return 7;}if(faults(invalid)!=2){return 8;}
            if(faults(err)!=2){return 9;}
            return 0;
        }""")
    print("Native truth passed: 125 transitions, signed fractional gates, velocity conditions, loops and error propagation; both optimization modes")


if __name__ == "__main__":
    main()

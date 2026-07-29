#!/bin/bash -eu

# JVM + native JNI build for tkr-allocation-hub (Jazzer / ClusterFuzzLite)

mkdir -p "$OUT"/native "$OUT"/libs

# Build product JAR with Gradle if available, else javac
if [ -f "./gradlew" ]; then
  ./gradlew jar -x test --no-daemon
  cp build/libs/tkr-allocation-hub-*.jar "$OUT"/tkr-allocation-hub.jar 2>/dev/null || \
    cp build/libs/*.jar "$OUT"/tkr-allocation-hub.jar
else
  find src/main/java -name '*.java' > sources.txt
  mkdir -p build/classes
  javac -encoding UTF-8 -d build/classes @sources.txt
  jar cf "$OUT"/tkr-allocation-hub.jar -C build/classes .
fi

PROJECT_JARS="tkr-allocation-hub.jar"
BUILD_CLASSPATH=$(echo $PROJECT_JARS | xargs printf -- "$OUT/%s:"):$JAZZER_API_PATH
RUNTIME_CLASSPATH=$(echo $PROJECT_JARS | xargs printf -- "\$this_dir/%s:"):\$this_dir:\$this_dir/native

# Native JNI library (ASAN UAF bug sites)
JVM_INCLUDES="-I${JAVA_HOME}/include -I${JAVA_HOME}/include/linux"
NATIVE_SAN="-fsanitize=address -fno-omit-frame-pointer"
$CXX $CXXFLAGS $NATIVE_SAN $JVM_INCLUDES -fPIC -shared \
  native/tkr_native.cpp \
  -o "$OUT"/native/libtkr_native.so

# Compile fuzz harnesses
for fuzzer in $(find src/fuzz/java -name '*Fuzzer.java'); do
  fuzzer_basename=$(basename -s .java "$fuzzer")
  javac -encoding UTF-8 -cp "$BUILD_CLASSPATH" -d "$OUT" "$fuzzer"

  driver=jazzer_driver_with_sanitizer
  echo "#!/bin/bash
# LLVMFuzzerTestOneInput for fuzzer detection.
this_dir=\$(dirname \"\$0\")
LD_LIBRARY_PATH=\"\$this_dir/native:\$JVM_LD_LIBRARY_PATH:\$this_dir\" \
ASAN_OPTIONS=\$ASAN_OPTIONS:symbolize=1:detect_leaks=0 \
\$this_dir/$driver --agent_path=\$this_dir/jazzer_agent_deploy.jar \
--cp=$RUNTIME_CLASSPATH \
--target_class=com.tkr.fuzz.$fuzzer_basename \
--jvm_args=\"-Xmx2048m:-Djava.awt.headless=true:-Djava.library.path=\$this_dir/native:-Dtkr.native.path=\$this_dir/native\" \
\$@" > "$OUT/$fuzzer_basename"
  chmod +x "$OUT/$fuzzer_basename"
done

echo "JVM build complete -> $OUT"

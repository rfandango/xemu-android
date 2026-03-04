#!/usr/bin/env sh

DEFAULT_JVM_OPTS=''

APP_HOME="$(cd "${0%/*}" && pwd -P)"
CLASSPATH="$APP_HOME/gradle/wrapper/gradle-wrapper.jar"

if [ -n "$JAVA_HOME" ] ; then
  JAVA_EXEC="$JAVA_HOME/bin/java"
else
  JAVA_EXEC=java
fi

exec "$JAVA_EXEC" $DEFAULT_JVM_OPTS \
  -Dorg.gradle.appname=gradlew \
  -classpath "$CLASSPATH" \
  org.gradle.wrapper.GradleWrapperMain "$@"

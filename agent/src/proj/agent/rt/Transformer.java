package proj.agent.rt;

import java.lang.instrument.ClassFileTransformer;
import java.security.ProtectionDomain;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

import proj.agent.asm.ClassReader;
import proj.agent.asm.ClassVisitor;
import proj.agent.asm.ClassWriter;
import proj.agent.asm.Label;
import proj.agent.asm.MethodVisitor;
import proj.agent.asm.Opcodes;
import proj.agent.asm.Type;

/**
 * Instruments application classes (packages in Options.include) with calls to {@link Trace}:
 *
 * <ul>
 *   <li>method entry, and every return: enter(id) / exit(id);</li>
 *   <li>a catch-all handler around each method body (not constructors): exitThrow(t, id), then
 *       rethrow, so an exception leaving the method is seen;</li>
 *   <li>before each athrow: thrown(t, site); at each exception handler: caught(t, site).</li>
 * </ul>
 *
 * The added handler needs a stack map frame. It declares only {@code this} and the parameters, which
 * holds for javac output; a method whose frames or stores disagree with that is left without the
 * handler (its exceptional exits are then inferred from the next event, as for constructors).
 */
final class Transformer implements ClassFileTransformer {
  static final String TRACE = "proj/agent/rt/Trace";
  private static final String[] REQUEST_TYPES = {
    "Lcom/sun/net/httpserver/HttpExchange;",
    "Ljavax/servlet/ServletRequest;", "Ljavax/servlet/http/HttpServletRequest;",
    "Ljakarta/servlet/ServletRequest;", "Ljakarta/servlet/http/HttpServletRequest;",
  };

  private final Options options;
  /** Classes this transformer changed, restored on detach. */
  final Set<String> transformed = ConcurrentHashMap.newKeySet();

  Transformer(Options options) {
    this.options = options;
  }

  @Override
  public byte[] transform(ClassLoader loader, String name, Class<?> redefining, ProtectionDomain domain, byte[] bytes) {
    // Bootstrap classes are never application code; generated proxies ($$) are skipped.
    if (name == null || loader == null || name.contains("$$") || !options.includes(name)) return null;
    try {
      ClassReader reader = new ClassReader(bytes);
      if ((reader.getAccess() & (Opcodes.ACC_INTERFACE | Opcodes.ACC_MODULE)) != 0) return null;
      ClassWriter writer = new ClassWriter(reader, ClassWriter.COMPUTE_MAXS);
      ClassAdapter adapter = new ClassAdapter(writer, Integer.toHexString(System.identityHashCode(loader)));
      reader.accept(adapter, ClassReader.EXPAND_FRAMES);
      if (adapter.methods == 0) return null;
      byte[] out = writer.toByteArray();
      transformed.add(name);
      AgentState.instrumented(adapter.methods);
      return out;
    } catch (Throwable t) {
      AgentState.transformFailed(name, t);
      return null;
    }
  }

  private static final class ClassAdapter extends ClassVisitor {
    private final String loaderKey;
    private String owner;
    private String source;
    private int version;
    int methods;

    ClassAdapter(ClassVisitor next, String loaderKey) {
      super(Opcodes.ASM9, next);
      this.loaderKey = loaderKey;
    }

    @Override
    public void visit(int version, int access, String name, String signature, String superName, String[] interfaces) {
      this.version = version & 0xFFFF;
      this.owner = name;
      super.visit(version, access, name, signature, superName, interfaces);
    }

    @Override
    public void visitSource(String source, String debug) {
      this.source = source;
      super.visitSource(source, debug);
    }

    @Override
    public MethodVisitor visitMethod(int access, String name, String desc, String signature, String[] exceptions) {
      MethodVisitor mv = super.visitMethod(access, name, desc, signature, exceptions);
      if (mv == null || (access & (Opcodes.ACC_ABSTRACT | Opcodes.ACC_NATIVE | Opcodes.ACC_BRIDGE)) != 0
          || name.startsWith("access$")) {
        return mv;
      }
      int id = Methods.method(loaderKey, owner, name, desc, source);
      methods++;
      return new MethodAdapter(mv, access, name, desc, id, owner, version >= Opcodes.V1_6);
    }
  }

  private static final class MethodAdapter extends MethodVisitor {
    private final int id;
    private final boolean constructor;
    private final boolean frames;
    private final Object[] initial;  // this + parameters, as frame types
    private final int[] slotKind;    // per parameter slot: load/store opcode kind (ILOAD..ALOAD), -1 for a long/double's 2nd slot
    private final int requestSlot;
    private final Label start = new Label();
    private final Map<Label, String> handlers = new HashMap<Label, String>();
    private boolean wrapSafe = true;
    private Label pendingCatch;
    private int line;
    private int ordinal;

    MethodAdapter(MethodVisitor next, int access, String name, String desc, int id, String owner, boolean frames) {
      super(Opcodes.ASM9, next);
      this.id = id;
      this.constructor = name.equals("<init>");
      this.frames = frames;
      boolean isStatic = (access & Opcodes.ACC_STATIC) != 0;
      Type[] args = Type.getArgumentTypes(desc);
      Object[] init = new Object[args.length + (isStatic ? 0 : 1)];
      int slots = isStatic ? 0 : 1;
      for (Type t : args) slots += t.getSize();
      int[] kinds = new int[slots];
      int i = 0;
      int slot = 0;
      if (!isStatic) {
        init[i++] = owner;
        kinds[slot++] = Opcodes.ALOAD;
      }
      int request = -1;
      for (Type t : args) {
        init[i++] = frameType(t);
        kinds[slot] = t.getOpcode(Opcodes.ILOAD);
        if (t.getSize() == 2) kinds[slot + 1] = -1;
        if (request < 0) {
          for (String r : REQUEST_TYPES) {
            if (r.equals(t.getDescriptor())) request = slot;
          }
        }
        slot += t.getSize();
      }
      this.initial = init;
      this.slotKind = kinds;
      this.requestSlot = request;
    }

    private static Object frameType(Type t) {
      switch (t.getSort()) {
        case Type.BOOLEAN:
        case Type.BYTE:
        case Type.CHAR:
        case Type.SHORT:
        case Type.INT:
          return Opcodes.INTEGER;
        case Type.FLOAT:
          return Opcodes.FLOAT;
        case Type.LONG:
          return Opcodes.LONG;
        case Type.DOUBLE:
          return Opcodes.DOUBLE;
        case Type.ARRAY:
          return t.getDescriptor();
        default:
          return t.getInternalName();
      }
    }

    private void push(int value) {
      if (value >= -1 && value <= 5) super.visitInsn(Opcodes.ICONST_0 + value);
      else if (value >= Byte.MIN_VALUE && value <= Byte.MAX_VALUE) super.visitIntInsn(Opcodes.BIPUSH, value);
      else if (value >= Short.MIN_VALUE && value <= Short.MAX_VALUE) super.visitIntInsn(Opcodes.SIPUSH, value);
      else super.visitLdcInsn(value);
    }

    private void call(String name, String desc) {
      super.visitMethodInsn(Opcodes.INVOKESTATIC, TRACE, name, desc, false);
    }

    @Override
    public void visitCode() {
      super.visitCode();
      push(id);
      call("enter", "(I)V");
      if (requestSlot >= 0) {
        super.visitVarInsn(Opcodes.ALOAD, requestSlot);
        call("request", "(Ljava/lang/Object;)V");
      }
      if (!constructor) super.visitLabel(start);
    }

    @Override
    public void visitTryCatchBlock(Label start, Label end, Label handler, String type) {
      handlers.put(handler, type == null ? "" : type);
      super.visitTryCatchBlock(start, end, handler, type);
    }

    @Override
    public void visitLabel(Label label) {
      super.visitLabel(label);
      if (handlers.containsKey(label)) pendingCatch = label;
    }

    @Override
    public void visitLineNumber(int line, Label start) {
      this.line = line;
      Methods.line(id, line);
      super.visitLineNumber(line, start);
    }

    @Override
    public void visitFrame(int type, int nLocal, Object[] local, int nStack, Object[] stack) {
      // The added handler's frame must agree with every frame it covers on this and the parameters.
      if (type == Opcodes.F_NEW) {
        if (nLocal < initial.length) {
          wrapSafe = false;
        } else {
          for (int i = 0; i < initial.length; i++) {
            if (!initial[i].equals(local[i])) wrapSafe = false;
          }
        }
      }
      super.visitFrame(type, nLocal, local, nStack, stack);
    }

    // Exception handler entry: the exception is on the stack.
    private void beforeInstruction() {
      if (pendingCatch == null) return;
      String type = handlers.get(pendingCatch);
      pendingCatch = null;
      int site = Methods.site(id, line, type.isEmpty() ? Methods.FINALLY : Methods.CATCH, type, ordinal++);
      super.visitInsn(Opcodes.DUP);
      push(site);
      call("caught", "(Ljava/lang/Throwable;I)V");
    }

    @Override
    public void visitInsn(int opcode) {
      beforeInstruction();
      if (opcode >= Opcodes.IRETURN && opcode <= Opcodes.RETURN) {
        push(id);
        call("exit", "(I)V");
      } else if (opcode == Opcodes.ATHROW) {
        super.visitInsn(Opcodes.DUP);
        push(Methods.site(id, line, Methods.THROW, "", ordinal++));
        call("thrown", "(Ljava/lang/Throwable;I)V");
      }
      super.visitInsn(opcode);
    }

    @Override
    public void visitVarInsn(int opcode, int var) {
      beforeInstruction();
      if (opcode >= Opcodes.ISTORE && opcode <= Opcodes.ASTORE && var < slotKind.length
          && slotKind[var] != opcode - (Opcodes.ISTORE - Opcodes.ILOAD)) {
        wrapSafe = false;  // a parameter slot reused for another type
      }
      super.visitVarInsn(opcode, var);
    }

    @Override
    public void visitIntInsn(int opcode, int operand) {
      beforeInstruction();
      super.visitIntInsn(opcode, operand);
    }

    @Override
    public void visitTypeInsn(int opcode, String type) {
      beforeInstruction();
      super.visitTypeInsn(opcode, type);
    }

    @Override
    public void visitFieldInsn(int opcode, String owner, String name, String desc) {
      beforeInstruction();
      super.visitFieldInsn(opcode, owner, name, desc);
    }

    @Override
    public void visitMethodInsn(int opcode, String owner, String name, String desc, boolean itf) {
      beforeInstruction();
      super.visitMethodInsn(opcode, owner, name, desc, itf);
    }

    @Override
    public void visitInvokeDynamicInsn(String name, String desc, proj.agent.asm.Handle bsm, Object... args) {
      beforeInstruction();
      super.visitInvokeDynamicInsn(name, desc, bsm, args);
    }

    @Override
    public void visitJumpInsn(int opcode, Label label) {
      beforeInstruction();
      super.visitJumpInsn(opcode, label);
    }

    @Override
    public void visitLdcInsn(Object value) {
      beforeInstruction();
      super.visitLdcInsn(value);
    }

    @Override
    public void visitIincInsn(int var, int increment) {
      beforeInstruction();
      super.visitIincInsn(var, increment);
    }

    @Override
    public void visitTableSwitchInsn(int min, int max, Label dflt, Label... labels) {
      beforeInstruction();
      super.visitTableSwitchInsn(min, max, dflt, labels);
    }

    @Override
    public void visitLookupSwitchInsn(Label dflt, int[] keys, Label[] labels) {
      beforeInstruction();
      super.visitLookupSwitchInsn(dflt, keys, labels);
    }

    @Override
    public void visitMultiANewArrayInsn(String desc, int dims) {
      beforeInstruction();
      super.visitMultiANewArrayInsn(desc, dims);
    }

    @Override
    public void visitMaxs(int maxStack, int maxLocals) {
      if (!constructor && wrapSafe) {
        // Appended last, so the method's own handlers keep precedence.
        Label handler = new Label();
        super.visitTryCatchBlock(start, handler, handler, "java/lang/Throwable");
        super.visitLabel(handler);
        if (frames) super.visitFrame(Opcodes.F_NEW, initial.length, initial, 1, new Object[] {"java/lang/Throwable"});
        super.visitInsn(Opcodes.DUP);
        push(id);
        call("exitThrow", "(Ljava/lang/Throwable;I)V");
        super.visitInsn(Opcodes.ATHROW);
      }
      super.visitMaxs(maxStack, maxLocals);
    }
  }
}

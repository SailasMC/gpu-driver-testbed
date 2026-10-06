/*
 * This file is auto-generated.  DO NOT MODIFY.
 * Using: /opt/android-sdk/build-tools/35.0.0/aidl --lang=java -I /root/shizuku-api/aidl/src/main/aidl -p /opt/android-sdk/platforms/android-34/framework.aidl -o /root/appsrc/src /root/shizuku-api/aidl/src/main/aidl/moe/shizuku/server/IShizukuService.aidl
 */
package moe.shizuku.server;
public interface IShizukuService extends android.os.IInterface
{
  /** Default implementation for IShizukuService. */
  public static class Default implements moe.shizuku.server.IShizukuService
  {
    @Override public int getVersion() throws android.os.RemoteException
    {
      return 0;
    }
    @Override public int getUid() throws android.os.RemoteException
    {
      return 0;
    }
    @Override public int checkPermission(java.lang.String permission) throws android.os.RemoteException
    {
      return 0;
    }
    @Override public moe.shizuku.server.IRemoteProcess newProcess(java.lang.String[] cmd, java.lang.String[] env, java.lang.String dir) throws android.os.RemoteException
    {
      return null;
    }
    @Override public java.lang.String getSELinuxContext() throws android.os.RemoteException
    {
      return null;
    }
    @Override public java.lang.String getSystemProperty(java.lang.String name, java.lang.String defaultValue) throws android.os.RemoteException
    {
      return null;
    }
    @Override public void setSystemProperty(java.lang.String name, java.lang.String value) throws android.os.RemoteException
    {
    }
    @Override public int addUserService(moe.shizuku.server.IShizukuServiceConnection conn, android.os.Bundle args) throws android.os.RemoteException
    {
      return 0;
    }
    @Override public int removeUserService(moe.shizuku.server.IShizukuServiceConnection conn, android.os.Bundle args) throws android.os.RemoteException
    {
      return 0;
    }
    @Override public void requestPermission(int requestCode) throws android.os.RemoteException
    {
    }
    @Override public boolean checkSelfPermission() throws android.os.RemoteException
    {
      return false;
    }
    @Override public boolean shouldShowRequestPermissionRationale() throws android.os.RemoteException
    {
      return false;
    }
    @Override public void attachApplication(moe.shizuku.server.IShizukuApplication application, android.os.Bundle args) throws android.os.RemoteException
    {
    }
    @Override public void exit() throws android.os.RemoteException
    {
    }
    @Override public void attachUserService(android.os.IBinder binder, android.os.Bundle options) throws android.os.RemoteException
    {
    }
    @Override public void dispatchPackageChanged(android.content.Intent intent) throws android.os.RemoteException
    {
    }
    @Override public boolean isHidden(int uid) throws android.os.RemoteException
    {
      return false;
    }
    @Override public void dispatchPermissionConfirmationResult(int requestUid, int requestPid, int requestCode, android.os.Bundle data) throws android.os.RemoteException
    {
    }
    @Override public int getFlagsForUid(int uid, int mask) throws android.os.RemoteException
    {
      return 0;
    }
    @Override public void updateFlagsForUid(int uid, int mask, int value) throws android.os.RemoteException
    {
    }
    @Override
    public android.os.IBinder asBinder() {
      return null;
    }
  }
  /** Local-side IPC implementation stub class. */
  public static abstract class Stub extends android.os.Binder implements moe.shizuku.server.IShizukuService
  {
    /** Construct the stub at attach it to the interface. */
    @SuppressWarnings("this-escape")
    public Stub()
    {
      this.attachInterface(this, DESCRIPTOR);
    }
    /**
     * Cast an IBinder object into an moe.shizuku.server.IShizukuService interface,
     * generating a proxy if needed.
     */
    public static moe.shizuku.server.IShizukuService asInterface(android.os.IBinder obj)
    {
      if ((obj==null)) {
        return null;
      }
      android.os.IInterface iin = obj.queryLocalInterface(DESCRIPTOR);
      if (((iin!=null)&&(iin instanceof moe.shizuku.server.IShizukuService))) {
        return ((moe.shizuku.server.IShizukuService)iin);
      }
      return new moe.shizuku.server.IShizukuService.Stub.Proxy(obj);
    }
    @Override public android.os.IBinder asBinder()
    {
      return this;
    }
    @Override public boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags) throws android.os.RemoteException
    {
      java.lang.String descriptor = DESCRIPTOR;
      if (code >= android.os.IBinder.FIRST_CALL_TRANSACTION && code <= android.os.IBinder.LAST_CALL_TRANSACTION) {
        data.enforceInterface(descriptor);
      }
      if (code == INTERFACE_TRANSACTION) {
        reply.writeString(descriptor);
        return true;
      }
      switch (code)
      {
        case TRANSACTION_getVersion:
        {
          int _result = this.getVersion();
          reply.writeNoException();
          reply.writeInt(_result);
          break;
        }
        case TRANSACTION_getUid:
        {
          int _result = this.getUid();
          reply.writeNoException();
          reply.writeInt(_result);
          break;
        }
        case TRANSACTION_checkPermission:
        {
          java.lang.String _arg0;
          _arg0 = data.readString();
          int _result = this.checkPermission(_arg0);
          reply.writeNoException();
          reply.writeInt(_result);
          break;
        }
        case TRANSACTION_newProcess:
        {
          java.lang.String[] _arg0;
          _arg0 = data.createStringArray();
          java.lang.String[] _arg1;
          _arg1 = data.createStringArray();
          java.lang.String _arg2;
          _arg2 = data.readString();
          moe.shizuku.server.IRemoteProcess _result = this.newProcess(_arg0, _arg1, _arg2);
          reply.writeNoException();
          reply.writeStrongInterface(_result);
          break;
        }
        case TRANSACTION_getSELinuxContext:
        {
          java.lang.String _result = this.getSELinuxContext();
          reply.writeNoException();
          reply.writeString(_result);
          break;
        }
        case TRANSACTION_getSystemProperty:
        {
          java.lang.String _arg0;
          _arg0 = data.readString();
          java.lang.String _arg1;
          _arg1 = data.readString();
          java.lang.String _result = this.getSystemProperty(_arg0, _arg1);
          reply.writeNoException();
          reply.writeString(_result);
          break;
        }
        case TRANSACTION_setSystemProperty:
        {
          java.lang.String _arg0;
          _arg0 = data.readString();
          java.lang.String _arg1;
          _arg1 = data.readString();
          this.setSystemProperty(_arg0, _arg1);
          reply.writeNoException();
          break;
        }
        case TRANSACTION_addUserService:
        {
          moe.shizuku.server.IShizukuServiceConnection _arg0;
          _arg0 = moe.shizuku.server.IShizukuServiceConnection.Stub.asInterface(data.readStrongBinder());
          android.os.Bundle _arg1;
          _arg1 = _Parcel.readTypedObject(data, android.os.Bundle.CREATOR);
          int _result = this.addUserService(_arg0, _arg1);
          reply.writeNoException();
          reply.writeInt(_result);
          break;
        }
        case TRANSACTION_removeUserService:
        {
          moe.shizuku.server.IShizukuServiceConnection _arg0;
          _arg0 = moe.shizuku.server.IShizukuServiceConnection.Stub.asInterface(data.readStrongBinder());
          android.os.Bundle _arg1;
          _arg1 = _Parcel.readTypedObject(data, android.os.Bundle.CREATOR);
          int _result = this.removeUserService(_arg0, _arg1);
          reply.writeNoException();
          reply.writeInt(_result);
          break;
        }
        case TRANSACTION_requestPermission:
        {
          int _arg0;
          _arg0 = data.readInt();
          this.requestPermission(_arg0);
          reply.writeNoException();
          break;
        }
        case TRANSACTION_checkSelfPermission:
        {
          boolean _result = this.checkSelfPermission();
          reply.writeNoException();
          reply.writeInt(((_result)?(1):(0)));
          break;
        }
        case TRANSACTION_shouldShowRequestPermissionRationale:
        {
          boolean _result = this.shouldShowRequestPermissionRationale();
          reply.writeNoException();
          reply.writeInt(((_result)?(1):(0)));
          break;
        }
        case TRANSACTION_attachApplication:
        {
          moe.shizuku.server.IShizukuApplication _arg0;
          _arg0 = moe.shizuku.server.IShizukuApplication.Stub.asInterface(data.readStrongBinder());
          android.os.Bundle _arg1;
          _arg1 = _Parcel.readTypedObject(data, android.os.Bundle.CREATOR);
          this.attachApplication(_arg0, _arg1);
          reply.writeNoException();
          break;
        }
        case TRANSACTION_exit:
        {
          this.exit();
          reply.writeNoException();
          break;
        }
        case TRANSACTION_attachUserService:
        {
          android.os.IBinder _arg0;
          _arg0 = data.readStrongBinder();
          android.os.Bundle _arg1;
          _arg1 = _Parcel.readTypedObject(data, android.os.Bundle.CREATOR);
          this.attachUserService(_arg0, _arg1);
          reply.writeNoException();
          break;
        }
        case TRANSACTION_dispatchPackageChanged:
        {
          android.content.Intent _arg0;
          _arg0 = _Parcel.readTypedObject(data, android.content.Intent.CREATOR);
          this.dispatchPackageChanged(_arg0);
          break;
        }
        case TRANSACTION_isHidden:
        {
          int _arg0;
          _arg0 = data.readInt();
          boolean _result = this.isHidden(_arg0);
          reply.writeNoException();
          reply.writeInt(((_result)?(1):(0)));
          break;
        }
        case TRANSACTION_dispatchPermissionConfirmationResult:
        {
          int _arg0;
          _arg0 = data.readInt();
          int _arg1;
          _arg1 = data.readInt();
          int _arg2;
          _arg2 = data.readInt();
          android.os.Bundle _arg3;
          _arg3 = _Parcel.readTypedObject(data, android.os.Bundle.CREATOR);
          this.dispatchPermissionConfirmationResult(_arg0, _arg1, _arg2, _arg3);
          break;
        }
        case TRANSACTION_getFlagsForUid:
        {
          int _arg0;
          _arg0 = data.readInt();
          int _arg1;
          _arg1 = data.readInt();
          int _result = this.getFlagsForUid(_arg0, _arg1);
          reply.writeNoException();
          reply.writeInt(_result);
          break;
        }
        case TRANSACTION_updateFlagsForUid:
        {
          int _arg0;
          _arg0 = data.readInt();
          int _arg1;
          _arg1 = data.readInt();
          int _arg2;
          _arg2 = data.readInt();
          this.updateFlagsForUid(_arg0, _arg1, _arg2);
          reply.writeNoException();
          break;
        }
        default:
        {
          return super.onTransact(code, data, reply, flags);
        }
      }
      return true;
    }
    private static class Proxy implements moe.shizuku.server.IShizukuService
    {
      private android.os.IBinder mRemote;
      Proxy(android.os.IBinder remote)
      {
        mRemote = remote;
      }
      @Override public android.os.IBinder asBinder()
      {
        return mRemote;
      }
      public java.lang.String getInterfaceDescriptor()
      {
        return DESCRIPTOR;
      }
      @Override public int getVersion() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        int _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_getVersion, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readInt();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public int getUid() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        int _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_getUid, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readInt();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public int checkPermission(java.lang.String permission) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        int _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeString(permission);
          boolean _status = mRemote.transact(Stub.TRANSACTION_checkPermission, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readInt();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public moe.shizuku.server.IRemoteProcess newProcess(java.lang.String[] cmd, java.lang.String[] env, java.lang.String dir) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        moe.shizuku.server.IRemoteProcess _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeStringArray(cmd);
          _data.writeStringArray(env);
          _data.writeString(dir);
          boolean _status = mRemote.transact(Stub.TRANSACTION_newProcess, _data, _reply, 0);
          _reply.readException();
          _result = moe.shizuku.server.IRemoteProcess.Stub.asInterface(_reply.readStrongBinder());
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public java.lang.String getSELinuxContext() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        java.lang.String _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_getSELinuxContext, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readString();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public java.lang.String getSystemProperty(java.lang.String name, java.lang.String defaultValue) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        java.lang.String _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeString(name);
          _data.writeString(defaultValue);
          boolean _status = mRemote.transact(Stub.TRANSACTION_getSystemProperty, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readString();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public void setSystemProperty(java.lang.String name, java.lang.String value) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeString(name);
          _data.writeString(value);
          boolean _status = mRemote.transact(Stub.TRANSACTION_setSystemProperty, _data, _reply, 0);
          _reply.readException();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
      }
      @Override public int addUserService(moe.shizuku.server.IShizukuServiceConnection conn, android.os.Bundle args) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        int _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeStrongInterface(conn);
          _Parcel.writeTypedObject(_data, args, 0);
          boolean _status = mRemote.transact(Stub.TRANSACTION_addUserService, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readInt();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public int removeUserService(moe.shizuku.server.IShizukuServiceConnection conn, android.os.Bundle args) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        int _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeStrongInterface(conn);
          _Parcel.writeTypedObject(_data, args, 0);
          boolean _status = mRemote.transact(Stub.TRANSACTION_removeUserService, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readInt();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public void requestPermission(int requestCode) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeInt(requestCode);
          boolean _status = mRemote.transact(Stub.TRANSACTION_requestPermission, _data, _reply, 0);
          _reply.readException();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
      }
      @Override public boolean checkSelfPermission() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        boolean _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_checkSelfPermission, _data, _reply, 0);
          _reply.readException();
          _result = (0!=_reply.readInt());
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public boolean shouldShowRequestPermissionRationale() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        boolean _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_shouldShowRequestPermissionRationale, _data, _reply, 0);
          _reply.readException();
          _result = (0!=_reply.readInt());
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public void attachApplication(moe.shizuku.server.IShizukuApplication application, android.os.Bundle args) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeStrongInterface(application);
          _Parcel.writeTypedObject(_data, args, 0);
          boolean _status = mRemote.transact(Stub.TRANSACTION_attachApplication, _data, _reply, 0);
          _reply.readException();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
      }
      @Override public void exit() throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          boolean _status = mRemote.transact(Stub.TRANSACTION_exit, _data, _reply, 0);
          _reply.readException();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
      }
      @Override public void attachUserService(android.os.IBinder binder, android.os.Bundle options) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeStrongBinder(binder);
          _Parcel.writeTypedObject(_data, options, 0);
          boolean _status = mRemote.transact(Stub.TRANSACTION_attachUserService, _data, _reply, 0);
          _reply.readException();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
      }
      @Override public void dispatchPackageChanged(android.content.Intent intent) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _Parcel.writeTypedObject(_data, intent, 0);
          boolean _status = mRemote.transact(Stub.TRANSACTION_dispatchPackageChanged, _data, null, android.os.IBinder.FLAG_ONEWAY);
        }
        finally {
          _data.recycle();
        }
      }
      @Override public boolean isHidden(int uid) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        boolean _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeInt(uid);
          boolean _status = mRemote.transact(Stub.TRANSACTION_isHidden, _data, _reply, 0);
          _reply.readException();
          _result = (0!=_reply.readInt());
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public void dispatchPermissionConfirmationResult(int requestUid, int requestPid, int requestCode, android.os.Bundle data) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeInt(requestUid);
          _data.writeInt(requestPid);
          _data.writeInt(requestCode);
          _Parcel.writeTypedObject(_data, data, 0);
          boolean _status = mRemote.transact(Stub.TRANSACTION_dispatchPermissionConfirmationResult, _data, null, android.os.IBinder.FLAG_ONEWAY);
        }
        finally {
          _data.recycle();
        }
      }
      @Override public int getFlagsForUid(int uid, int mask) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        int _result;
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeInt(uid);
          _data.writeInt(mask);
          boolean _status = mRemote.transact(Stub.TRANSACTION_getFlagsForUid, _data, _reply, 0);
          _reply.readException();
          _result = _reply.readInt();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
        return _result;
      }
      @Override public void updateFlagsForUid(int uid, int mask, int value) throws android.os.RemoteException
      {
        android.os.Parcel _data = android.os.Parcel.obtain();
        android.os.Parcel _reply = android.os.Parcel.obtain();
        try {
          _data.writeInterfaceToken(DESCRIPTOR);
          _data.writeInt(uid);
          _data.writeInt(mask);
          _data.writeInt(value);
          boolean _status = mRemote.transact(Stub.TRANSACTION_updateFlagsForUid, _data, _reply, 0);
          _reply.readException();
        }
        finally {
          _reply.recycle();
          _data.recycle();
        }
      }
    }
    static final int TRANSACTION_getVersion = (android.os.IBinder.FIRST_CALL_TRANSACTION + 2);
    static final int TRANSACTION_getUid = (android.os.IBinder.FIRST_CALL_TRANSACTION + 3);
    static final int TRANSACTION_checkPermission = (android.os.IBinder.FIRST_CALL_TRANSACTION + 4);
    static final int TRANSACTION_newProcess = (android.os.IBinder.FIRST_CALL_TRANSACTION + 7);
    static final int TRANSACTION_getSELinuxContext = (android.os.IBinder.FIRST_CALL_TRANSACTION + 8);
    static final int TRANSACTION_getSystemProperty = (android.os.IBinder.FIRST_CALL_TRANSACTION + 9);
    static final int TRANSACTION_setSystemProperty = (android.os.IBinder.FIRST_CALL_TRANSACTION + 10);
    static final int TRANSACTION_addUserService = (android.os.IBinder.FIRST_CALL_TRANSACTION + 11);
    static final int TRANSACTION_removeUserService = (android.os.IBinder.FIRST_CALL_TRANSACTION + 12);
    static final int TRANSACTION_requestPermission = (android.os.IBinder.FIRST_CALL_TRANSACTION + 14);
    static final int TRANSACTION_checkSelfPermission = (android.os.IBinder.FIRST_CALL_TRANSACTION + 15);
    static final int TRANSACTION_shouldShowRequestPermissionRationale = (android.os.IBinder.FIRST_CALL_TRANSACTION + 16);
    static final int TRANSACTION_attachApplication = (android.os.IBinder.FIRST_CALL_TRANSACTION + 17);
    static final int TRANSACTION_exit = (android.os.IBinder.FIRST_CALL_TRANSACTION + 100);
    static final int TRANSACTION_attachUserService = (android.os.IBinder.FIRST_CALL_TRANSACTION + 101);
    static final int TRANSACTION_dispatchPackageChanged = (android.os.IBinder.FIRST_CALL_TRANSACTION + 102);
    static final int TRANSACTION_isHidden = (android.os.IBinder.FIRST_CALL_TRANSACTION + 103);
    static final int TRANSACTION_dispatchPermissionConfirmationResult = (android.os.IBinder.FIRST_CALL_TRANSACTION + 104);
    static final int TRANSACTION_getFlagsForUid = (android.os.IBinder.FIRST_CALL_TRANSACTION + 105);
    static final int TRANSACTION_updateFlagsForUid = (android.os.IBinder.FIRST_CALL_TRANSACTION + 106);
  }
  /** @hide */
  public static final java.lang.String DESCRIPTOR = "moe.shizuku.server.IShizukuService";
  public int getVersion() throws android.os.RemoteException;
  public int getUid() throws android.os.RemoteException;
  public int checkPermission(java.lang.String permission) throws android.os.RemoteException;
  public moe.shizuku.server.IRemoteProcess newProcess(java.lang.String[] cmd, java.lang.String[] env, java.lang.String dir) throws android.os.RemoteException;
  public java.lang.String getSELinuxContext() throws android.os.RemoteException;
  public java.lang.String getSystemProperty(java.lang.String name, java.lang.String defaultValue) throws android.os.RemoteException;
  public void setSystemProperty(java.lang.String name, java.lang.String value) throws android.os.RemoteException;
  public int addUserService(moe.shizuku.server.IShizukuServiceConnection conn, android.os.Bundle args) throws android.os.RemoteException;
  public int removeUserService(moe.shizuku.server.IShizukuServiceConnection conn, android.os.Bundle args) throws android.os.RemoteException;
  public void requestPermission(int requestCode) throws android.os.RemoteException;
  public boolean checkSelfPermission() throws android.os.RemoteException;
  public boolean shouldShowRequestPermissionRationale() throws android.os.RemoteException;
  public void attachApplication(moe.shizuku.server.IShizukuApplication application, android.os.Bundle args) throws android.os.RemoteException;
  public void exit() throws android.os.RemoteException;
  public void attachUserService(android.os.IBinder binder, android.os.Bundle options) throws android.os.RemoteException;
  public void dispatchPackageChanged(android.content.Intent intent) throws android.os.RemoteException;
  public boolean isHidden(int uid) throws android.os.RemoteException;
  public void dispatchPermissionConfirmationResult(int requestUid, int requestPid, int requestCode, android.os.Bundle data) throws android.os.RemoteException;
  public int getFlagsForUid(int uid, int mask) throws android.os.RemoteException;
  public void updateFlagsForUid(int uid, int mask, int value) throws android.os.RemoteException;
  /** @hide */
  static class _Parcel {
    static private <T> T readTypedObject(
        android.os.Parcel parcel,
        android.os.Parcelable.Creator<T> c) {
      if (parcel.readInt() != 0) {
          return c.createFromParcel(parcel);
      } else {
          return null;
      }
    }
    static private <T extends android.os.Parcelable> void writeTypedObject(
        android.os.Parcel parcel, T value, int parcelableFlags) {
      if (value != null) {
        parcel.writeInt(1);
        value.writeToParcel(parcel, parcelableFlags);
      } else {
        parcel.writeInt(0);
      }
    }
  }
}

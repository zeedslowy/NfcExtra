package tud.seemuh.nfcgate.network.threading;

import android.util.Log;

import java.io.DataInputStream;
import java.io.IOException;
import java.net.Socket;

import tud.seemuh.nfcgate.network.NetworkStatus;
import tud.seemuh.nfcgate.network.ServerConnection;

public class ReceiveThread extends BaseThread {
    private static final String TAG = "ReceiveThread";

    // references
    private ServerConnection mConnection;
    private DataInputStream mReadStream;

    /**
     * Waits on sendQueue and sends the data over the specified stream
     */
    public ReceiveThread(ServerConnection connection) {
        super();
        mConnection = connection;
    }

    @Override
    void initThread() throws IOException {
        Socket socket = mConnection.getSocket();

        if (socket == null)
            throw new IOException("Socket error");
        else
            mReadStream = new DataInputStream(socket.getInputStream());
    }

    /**
     * Tries to send one item from the sendQueue.
     */
    @Override
    void runInternal() throws IOException {

        // block and wait for the 4 byte length prefix
        int length = mReadStream.readInt();

        // block and wait for actual data
        byte[] data = new byte[length];
        mReadStream.readFully(data);

        Log.v(TAG, "Got message of " + length + " bytes");

        // deliver data
        mConnection.onReceive(data);
    }

    @Override
    void onError(Exception e) {
        Log.e(TAG, "Receive onError", e);
        mConnection.reportStatus(NetworkStatus.ERROR);
    }
}
